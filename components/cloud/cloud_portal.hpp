#pragma once

// Captive-portal provisioning: brings up the device's own SoftAP, a DNS responder
// that points every lookup at the device, and an HTTP server that serves a single
// setup form (Wi-Fi network + password + cloud endpoint URL + token). The phone
// joins the temp AP, the OS captive-portal probe auto-pops the form, and the
// parsed fields are handed back to cloud.cpp via the SubmitCb. No companion app,
// no serial — the browser the user already has is the whole client.
//
// This is the IDF-bound half of provisioning; cloud.cpp owns the Wi-Fi lifecycle
// (mode/start/connect) and NVS. The token crosses the local AP hop in cleartext —
// an accepted tradeoff for a short, user-initiated, on-prem provisioning window.

#include "esp_err.h"

namespace espressopost::cloud::portal {

// Fields parsed from the setup form. Sized to the protocol maxima: SSID 32 + NUL,
// WPA passphrase 64 + NUL; url/token match cloud.cpp's NVS limits. A field left
// blank in the form arrives empty, meaning "keep the current value" (cloud.cpp).
struct Submission {
  char ssid[33];
  char password[65];
  char url[160];
  char token[64];
};

// Invoked from the httpd task when the user submits a valid form. The callback
// should persist/apply the fields and kick the STA connect; it must not call
// stop() (that would tear down the task it runs on — cloud.cpp defers teardown to
// the GOT_IP event instead).
using SubmitCb = void (*)(const Submission&);

// Prefill the form's endpoint-URL field for the next start() (e.g. the current
// stored URL, so re-running setup needn't retype it). Not the token — that's
// never echoed back. Pass "" / nullptr to clear. Call before start().
void set_prefill(const char* url);

// Configure the SoftAP `ap_ssid` (open), start the DNS responder and the HTTP
// server. The radio must already be in WIFI_MODE_APSTA and started (cloud.cpp
// does that, since it owns the STA side and a lazy network scan in the form needs
// STA up). Idempotent-safe to follow with stop(). Returns the first failing step.
esp_err_t start(const char* ap_ssid, SubmitCb cb);

// Stop the HTTP server and the DNS responder and release the socket. Safe to call
// when not started. Does not touch Wi-Fi mode — the caller restores that.
void stop();

}  // namespace espressopost::cloud::portal
