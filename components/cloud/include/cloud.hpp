#pragma once

// Cloud sync service: connects to WiFi (credentials received over SoftAP
// provisioning — the wifi_provisioning manager; no network list or password
// field on the device, the phone app carries those) and mirrors every saved
// shot into a Google Sheet via a Google Apps Script Web App. Upload
// is a durable queue + backfill: a high-water mark in NVS tracks the last
// uploaded record, and a background task uploads everything above it whenever
// WiFi is up, so offline pulls, reboots, and existing history all sync.
//
// Shaped like the climate component: non-fatal init(), a core-0 background task,
// and a mutex-guarded status snapshot the UI reads. The endpoint URL + shared
// token live in NVS, set over the serial console (`cloud set-url`/`set-token`).

#include <cstdint>

#include "esp_err.h"

namespace espressopost::cloud {

enum class WifiState : uint8_t {
  Disabled,      // no stored creds; idle until provisioning
  Provisioning,  // SoftAP up, waiting for creds from the phone app
  Connecting,    // associating / awaiting IP
  Connected,     // got an IP
  Failed,        // gave up reconnecting after repeated attempts
};

enum class SyncState : uint8_t {
  Idle,      // nothing queued (or not connected/configured)
  Syncing,   // a POST is in flight
  Backoff,   // last POST failed; waiting before retry
  Error,     // reserved; currently folded into Backoff
};

struct Status {
  WifiState wifi;
  SyncState sync;
  uint32_t  synced_count;   // records confirmed uploaded (== persisted HWM)
  uint32_t  pending_count;  // records on disk above the HWM
  int8_t    rssi_dbm;       // last AP RSSI, 0 if unknown
  esp_err_t last_error;     // last sync/HTTP error, ESP_OK if none
  bool      configured;     // endpoint URL + token both present in NVS
  // SoftAP provisioning identity, populated only while wifi == Provisioning:
  // the temporary AP to join and the PoP to enter in the phone app.
  char      prov_ssid[24];
  char      prov_pop[12];
};

// Bring up netif/event-loop/WiFi (STA), load the endpoint + HWM from NVS,
// register the serial console commands, and spawn the sync task. Auto-connects
// if WiFi creds are already stored. Non-fatal — app_main treats a failure like
// climate/rtc (warn + continue); shots just stay queued locally.
esp_err_t init();

// Begin / abort a SoftAP provisioning session (driven by a UI button or the
// `cloud provision` console command).
esp_err_t start_provisioning();
void      cancel_provisioning();

// Mutex-guarded snapshot for the UI. Cheap; safe to call from the LVGL task.
Status status();

// Nudge the sync task that a new shot was appended (called from on_submit after
// a successful append). Lock-free — just sets an event-group bit; safe from the
// LVGL task.
void notify_new_shot();

}  // namespace espressopost::cloud
