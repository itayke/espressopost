# Cloud sink — Google Apps Script Web App

The device's cloud sync (`components/cloud/`) POSTs saved shots as JSON to a
Google Apps Script Web App, which appends one row per shot to a Sheet. This is
the "simple Google cloud script into a sheet" sink — no server to run, no
billing, just a bound script on a Sheet you own.

The device holds the deployment URL + a shared token in NVS (entered in the
captive-portal setup form, or over the serial console — see below), never in the
firmware source.

## 1. Create the Sheet + script

1. New Google Sheet (this becomes the log).
2. **Extensions ▸ Apps Script** — opens a bound script project.
3. Replace `Code.gs` with the script below.
4. Set `TOKEN` to a long random string (this is the shared secret the device
   sends; treat it like a password).

```js
// Bound to the destination Sheet. Appends one row per posted shot.
const TOKEN = 'PUT-A-LONG-RANDOM-STRING-HERE';   // must match `cloud set-token`
const SHEET = 'shots';

// Column order written to the sheet. `index` is the device's absolute record
// number — used to dedupe so a retried POST (e.g. a 200 lost in transit after
// the device already advanced its high-water mark) can't double-write a row.
const HEADERS = [
  'index', 'epoch', 'datetime', 'boot_us', 'preset', 'time_s', 'stars', 'conf',
  'taste', 'anomaly', 'tombstone', 'temp_c', 'rh', 'hpa', 'grind', 'suggested',
];

function doPost(e) {
  try {
    const body = JSON.parse(e.postData.contents);
    if (body.token !== TOKEN) {
      return json_({ ok: false, error: 'bad token' });
    }
    const ss = SpreadsheetApp.getActiveSpreadsheet();
    let sh = ss.getSheetByName(SHEET);
    if (!sh) {
      sh = ss.insertSheet(SHEET);
      sh.appendRow(HEADERS);
    }

    // Existing indices (column A, minus the header) for idempotent dedupe.
    const lastRow = sh.getLastRow();
    const seen = {};
    if (lastRow > 1) {
      const col = sh.getRange(2, 1, lastRow - 1, 1).getValues();
      for (let i = 0; i < col.length; i++) seen[col[i][0]] = true;
    }

    let written = 0;
    (body.shots || []).forEach(function (s) {
      if (seen[s.index]) return;  // already have this record
      // `datetime` is a real Date value derived from epoch (UTC instant; the sheet
      // displays it in its own timezone). Blank when epoch is 0/missing — a shot
      // logged before the RTC synced — so it reads empty, not 1970; `boot_us`
      // remains the fallback ordering key for those. `epoch` stays the source of truth.
      const dt = s.epoch ? new Date(s.epoch * 1000) : '';
      sh.appendRow([
        s.index, s.epoch || '', dt, s.boot_us, s.preset, s.time_s, s.stars, s.conf,
        (s.taste || []).join('|'),
        s.anomaly, s.tombstone,
        s.temp_c, s.rh, s.hpa, s.grind,
        ('suggested' in s) ? s.suggested : '',
      ]);
      seen[s.index] = true;
      written++;
    });
    return json_({ ok: true, written: written });
  } catch (err) {
    return json_({ ok: false, error: String(err) });
  }
}

function json_(obj) {
  return ContentService.createTextOutput(JSON.stringify(obj))
    .setMimeType(ContentService.MimeType.JSON);
}
```

## 2. Deploy as a Web App

1. **Deploy ▸ New deployment ▸** type **Web app**.
2. **Execute as: Me** (so the script can write your Sheet).
3. **Who has access: Anyone** (the device sends no Google credentials; the
   `TOKEN` check is what gates writes).
4. Copy the **`/exec` URL** (looks like
   `https://script.google.com/macros/s/AKfy.../exec`).

Re-deploying for a code change can mint a new `/exec` URL — that's exactly why
the URL lives in NVS (set it again with the command below), not in firmware.

## 3. Point the device at it

**Menu ▸ Connections ▸ Connect Wi-Fi.** The screen shows a Wi-Fi-join QR for a
temporary setup network (`EP SETUP xxxxxx`). Scan it (the phone's camera offers "Join
network") or join that open AP from Wi-Fi settings; the device's setup page opens
automatically. The single form takes your **Wi-Fi network + password** *and* the
**endpoint URL + token** below — paste the `/exec` URL and your `TOKEN`, submit,
and the device connects, backfills any unsynced shots, and uploads each new one on
submit. No app, no serial.

```
url:   https://script.google.com/macros/s/AKfy.../exec
token: PUT-A-LONG-RANDOM-STRING-HERE
```

Serial fallback (`idf.py monitor`, prompt `esp>`) for recovery/headless setup:

```
cloud set-url https://script.google.com/macros/s/AKfy.../exec
cloud set-token PUT-A-LONG-RANDOM-STRING-HERE
cloud status          # shows wifi/sync state; token is never echoed back
cloud sync            # force an immediate upload attempt
cloud resync          # rewind to index 0 and re-upload every shot (see below)
```

If you clear or recreate the Sheet, the device won't re-send the old shots on its
own — it tracks a **high-water mark** in NVS and only uploads records above it.
Run `cloud resync` to rewind that mark to 0 and re-upload everything from scratch;
the script's `index` dedupe means rows that still exist won't be duplicated.

## Notes / gotchas

- **Why the token, not auth:** a Web App set to "Anyone" needs no OAuth from the
  device (which has no browser), so the shared token is the access control.
  Rotate it by editing `TOKEN`, redeploying, and re-submitting the setup form
  (or `cloud set-token`).
- **The 302 redirect:** Apps Script answers `/exec` with a 302 to
  `script.googleusercontent.com`; `doPost` runs on the first hop (with the body)
  and the device's HTTP client follows the redirect to read the result. The
  firmware validates TLS for both hosts via the bundled Google roots
  (`CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`).
- **Status is always 200:** ContentService can't set HTTP status codes, so the
  device treats a shot as uploaded only when the response body contains
  `"ok":true`. A `bad token` (still HTTP 200) leaves the high-water mark put and
  the device retries with backoff.
- **The `datetime` column:** a human-readable mirror of `epoch`, written as a real
  Date value (not a string) so it sorts, filters, and charts natively. It's
  displayed in the **spreadsheet's** timezone — set it once under File ▸ Settings ▸
  Time zone, then restyle the format anytime via Format ▸ Number without redeploying.
  `epoch` stays the unambiguous UTC source of truth.
- **Quotas:** Apps Script has daily script-runtime limits and cold-start latency
  (the device uses a 15 s timeout); the device batches up to 20 shots per POST
  to stay well under them.
