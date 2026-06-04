#pragma once

// Cloud sync service: connects to WiFi (credentials captured by an on-device
// captive-portal web form — the device hosts its own SoftAP + setup page, so
// there's no network list or password field on the 466px screen and no companion
// app) and mirrors every saved shot into a Google Sheet via a Google Apps Script
// Web App. Upload is a durable queue + backfill: a high-water mark in NVS tracks
// the last uploaded record, and a background task uploads everything above it
// whenever WiFi is up, so offline pulls, reboots, and existing history all sync.
//
// The captive-portal form also captures the endpoint URL + shared token, so the
// whole device is configured from a phone browser; the serial console
// (`cloud set-url`/`set-token`) remains as a recovery/headless fallback.
//
// Shaped like the climate component: non-fatal init(), a core-0 background task,
// and a mutex-guarded status snapshot the UI reads.

#include <cstdint>

#include "esp_err.h"

namespace espressopost::cloud {

enum class WifiState : uint8_t {
  Disabled,      // no stored creds; idle until provisioning
  Provisioning,  // SoftAP + captive-portal form up, waiting for a submission
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
  char      net_ssid[33];   // network joined while wifi == Connected ("" otherwise)
  // SoftAP name, populated only while wifi == Provisioning: the temporary setup
  // network to join (shown as a Wi-Fi-join QR + text on the Connections screen).
  char      prov_ssid[24];
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

// Drop the current connection and erase the stored Wi-Fi credentials so the
// device won't auto-reconnect (state → Disabled). The cloud endpoint URL/token
// are left intact. Driven by the Connections "Forget Network" control.
void      forget();

// Mutex-guarded snapshot for the UI. Cheap; safe to call from the LVGL task.
Status status();

// Nudge the sync task that a new shot was appended (called from on_submit after
// a successful append). Lock-free — just sets an event-group bit; safe from the
// LVGL task.
void notify_new_shot();

}  // namespace espressopost::cloud
