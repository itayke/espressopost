#include "cloud.hpp"

#include "cloud_json.hpp"
#include "cloud_portal.hpp"
#include "rtc.hpp"
#include "storage.hpp"

#include "esp_check.h"
#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>  // strcasecmp (case-insensitive Location header match)
#include <sys/time.h>  // gettimeofday — read the SNTP-set clock to seed the RTC

namespace espressopost::cloud {
namespace {

constexpr const char* kTag = "cloud";

// The pure JSON layer redefines the storage flag/taste bit positions so it can
// stay IDF-free. Assert the two definitions agree, so a future bit added on one
// side can't silently desync the wire format from the on-disk record.
static_assert(json::bits::kFlagTombstone == storage::kFlagTombstone, "flag bit drift");
static_assert(json::bits::kFlagAnomaly   == storage::kFlagAnomaly,   "flag bit drift");
static_assert(json::bits::kTasteBitter     == storage::kTasteBitter,     "taste bit drift");
static_assert(json::bits::kTasteSour       == storage::kTasteSour,       "taste bit drift");
static_assert(json::bits::kTasteAstringent == storage::kTasteAstringent, "taste bit drift");
static_assert(json::bits::kTasteWatery     == storage::kTasteWatery,     "taste bit drift");
static_assert(json::bits::kTasteChanneled  == storage::kTasteChanneled,  "taste bit drift");
static_assert(json::bits::kTasteBalanced   == storage::kTasteBalanced,   "taste bit drift");

// --- NVS (namespace "cloud"): endpoint URL, shared token, and the upload
// high-water mark. Single-letter keys keep nvs dumps readable, matching the
// climate component's convention. ---
constexpr const char* kNvsNamespace = "cloud";
constexpr const char* kNvsKeyUrl    = "u";
constexpr const char* kNvsKeyToken  = "k";
constexpr const char* kNvsKeyHwm    = "hwm";

// --- Tunables (documented, in one place per house style). ---
constexpr size_t   kUrlMax        = 160;     // Apps Script /exec URL fits in ~120
constexpr size_t   kTokenMax      = 64;
constexpr size_t   kBatchSize     = 20;      // records per POST
constexpr uint32_t kSyncTaskStack = 12288;   // mbedTLS handshake needs the headroom
constexpr UBaseType_t kSyncTaskPrio = 3;     // same as climate
constexpr BaseType_t  kSyncTaskCore = 0;     // off the LVGL core (1)
constexpr int      kHttpTimeoutMs = 15000;   // Apps Script cold starts are slow
constexpr uint32_t kBackoffMinMs  = 5000;
constexpr uint32_t kBackoffMaxMs  = 300000;
constexpr uint32_t kBackoffFactor = 2;
constexpr int      kWifiMaxRetry  = 6;       // app-level reconnect attempts
constexpr size_t   kRespCapBytes  = 256;     // response prefix we keep to check "ok":true

// Event-group bits driving the sync task.
constexpr EventBits_t kBitGotIp       = 1u << 0;
constexpr EventBits_t kBitNewShot     = 1u << 1;
constexpr EventBits_t kBitProvConnect = 1u << 2;  // form submitted: drop AP, connect STA
constexpr EventBits_t kBitTimeSynced  = 1u << 3;  // SNTP got real UTC: push it to the RTC

// --- State (guarded by s_mutex unless noted). ---
SemaphoreHandle_t  s_mutex  = nullptr;
EventGroupHandle_t s_events = nullptr;
TaskHandle_t       s_task   = nullptr;
esp_netif_t*       s_netif  = nullptr;

WifiState s_wifi       = WifiState::Disabled;
SyncState s_sync       = SyncState::Idle;
esp_err_t s_last_error = ESP_OK;
int8_t    s_rssi       = 0;
uint32_t  s_hwm        = 0;
int       s_wifi_retry = 0;
bool      s_started      = false;  // esp_wifi_start() has been called
bool      s_sntp_inited  = false;  // esp_netif_sntp_init() has run (once, on first GOT_IP)
bool      s_provisioning = false;  // captive portal (SoftAP + form) is up
bool      s_prov_submitted = false;  // form submitted; STA connect attempt in flight

char s_url[kUrlMax]     = {};
char s_token[kTokenMax] = {};

// SSID of the network the STA is joined to (captured at GOT_IP), for the
// Connections status line. Cleared on disconnect.
char s_net_ssid[33] = {};

// SoftAP name, derived from the WiFi MAC at provisioning start and shown on the
// Connections screen (as a Wi-Fi-join QR + text) so the user can join the
// temporary setup network. No PoP: the captive-portal form is the channel, not
// the security1 handshake the old wifi_provisioning manager used.
char s_prov_ssid[24] = {};

// --- small locked accessors ---
struct Lock {
  bool ok;
  explicit Lock(TickType_t t = portMAX_DELAY) : ok(xSemaphoreTake(s_mutex, t) == pdTRUE) {}
  ~Lock() { if (ok) xSemaphoreGive(s_mutex); }
};

void set_wifi_state(WifiState w) { Lock l; s_wifi = w; }
void set_sync_state(SyncState s) { Lock l; s_sync = s; }

bool is_connected() { Lock l; return s_wifi == WifiState::Connected; }
bool configured()   { Lock l; return s_url[0] != '\0' && s_token[0] != '\0'; }

bool has_stored_creds() {
  wifi_config_t cfg = {};
  if (esp_wifi_get_config(WIFI_IF_STA, &cfg) != ESP_OK) return false;
  return cfg.sta.ssid[0] != 0;
}

void load_config() {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) return;
  size_t len = sizeof(s_url);
  if (nvs_get_str(h, kNvsKeyUrl, s_url, &len) != ESP_OK) s_url[0] = '\0';
  len = sizeof(s_token);
  if (nvs_get_str(h, kNvsKeyToken, s_token, &len) != ESP_OK) s_token[0] = '\0';
  uint32_t hwm = 0;
  if (nvs_get_u32(h, kNvsKeyHwm, &hwm) == ESP_OK) s_hwm = hwm;
  nvs_close(h);
}

void persist_hwm(uint32_t hwm) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_u32(h, kNvsKeyHwm, hwm);
  nvs_commit(h);
  nvs_close(h);
}

bool persist_str(const char* key, const char* val, char* dst, size_t dst_sz) {
  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READWRITE, &h) != ESP_OK) return false;
  const esp_err_t e = nvs_set_str(h, key, val);
  if (e == ESP_OK) nvs_commit(h);
  nvs_close(h);
  if (e != ESP_OK) return false;
  Lock l;
  std::snprintf(dst, dst_sz, "%s", val);
  return true;
}

// --- HTTP ---

struct RespCtx {
  char   buf[kRespCapBytes];
  size_t len;
  char   location[512];  // Location header of a redirect, captured to follow manually
};

esp_err_t http_event(esp_http_client_event_t* evt) {
  auto* ctx = static_cast<RespCtx*>(evt->user_data);
  if (ctx == nullptr) return ESP_OK;
  if (evt->event_id == HTTP_EVENT_ON_HEADER) {
    if (evt->header_key != nullptr && strcasecmp(evt->header_key, "Location") == 0) {
      std::snprintf(ctx->location, sizeof(ctx->location), "%s", evt->header_value);
    }
  } else if (evt->event_id == HTTP_EVENT_ON_DATA) {
    if (ctx->len + 1 < sizeof(ctx->buf)) {
      size_t space = sizeof(ctx->buf) - 1 - ctx->len;
      size_t n = (static_cast<size_t>(evt->data_len) < space)
                     ? static_cast<size_t>(evt->data_len) : space;
      std::memcpy(ctx->buf + ctx->len, evt->data, n);
      ctx->len += n;
      ctx->buf[ctx->len] = '\0';
    }
  }
  return ESP_OK;
}

// POST `body` to `url`. On transport success, writes the HTTP status code to
// *out_status and the (truncated) response body into resp. Apps Script /exec
// answers with a 302 to a script.googleusercontent.com/macros/echo URL that
// serves the doPost result and accepts only GET. We must follow it manually as a
// GET: esp_http_client's auto-redirect preserves the POST method, so the echo
// host rejects it with 405. doPost already ran with the body on the first hop, so
// the echo URL holds the result. The cert bundle covers both hosts.
esp_err_t http_post(const char* url, const std::string& body, int* out_status, RespCtx* resp) {
  esp_http_client_config_t cfg = {};
  cfg.url                   = url;
  cfg.method                = HTTP_METHOD_POST;
  cfg.timeout_ms            = kHttpTimeoutMs;
  cfg.crt_bundle_attach     = esp_crt_bundle_attach;
  cfg.event_handler         = http_event;
  cfg.user_data             = resp;
  cfg.disable_auto_redirect = true;  // follow the 302 ourselves, as GET (see above)

  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr) return ESP_FAIL;

  esp_http_client_set_header(c, "Content-Type", "application/json");
  esp_http_client_set_post_field(c, body.data(), static_cast<int>(body.size()));

  esp_err_t err = esp_http_client_perform(c);
  int status = (err == ESP_OK) ? esp_http_client_get_status_code(c) : 0;

  // Follow a single redirect as GET. The 302 body is the throwaway redirect page,
  // so reset resp before fetching the real result from the echo URL.
  if (err == ESP_OK && (status == 301 || status == 302 || status == 303) &&
      resp->location[0] != '\0') {
    resp->len = 0;
    resp->buf[0] = '\0';
    esp_http_client_set_url(c, resp->location);
    esp_http_client_set_method(c, HTTP_METHOD_GET);
    esp_http_client_set_post_field(c, nullptr, 0);
    err = esp_http_client_perform(c);
    if (err == ESP_OK) status = esp_http_client_get_status_code(c);
  }

  if (err == ESP_OK) *out_status = status;
  esp_http_client_cleanup(c);
  return err;
}

// --- sync task ---

enum class BatchResult { Ok, UpToDate, Failed };

json::ShotJson to_json(const storage::ShotRecord& r, uint32_t index) {
  json::ShotJson j{};
  j.index           = index;
  j.version         = r.version;
  j.preset_id       = r.preset_id;
  j.actual_time_s   = r.actual_time_s;
  j.quality_stars   = r.quality_stars;
  j.flags           = r.flags;
  j.confidence_pct  = r.confidence_pct;
  j.taste_flags     = r.taste_flags;
  j.timestamp_us    = r.timestamp_us;
  j.rtc_epoch_s     = r.rtc_epoch_s;
  j.temp_c          = r.temp_c;
  j.humidity_pct    = r.humidity_pct;
  j.pressure_hpa    = r.pressure_hpa;
  j.user_grind      = r.user_grind;
  j.suggested_grind = r.suggested_grind;
  return j;
}

// Upload the next batch of records above the HWM. Reads records under storage's
// lock into a heap buffer, builds the payload string, frees the buffer, then
// POSTs — so storage's mutex is released and the record buffer is gone before
// the stack-heavy TLS handshake runs.
BatchResult upload_batch() {
  uint32_t hwm;
  char url[kUrlMax];
  char token[kTokenMax];
  {
    Lock l;
    hwm = s_hwm;
    std::snprintf(url, sizeof(url), "%s", s_url);
    std::snprintf(token, sizeof(token), "%s", s_token);
  }

  const uint32_t total = storage::shot_count();
  if (hwm >= total) return BatchResult::UpToDate;

  auto* recs = static_cast<storage::ShotRecord*>(
      std::malloc(sizeof(storage::ShotRecord) * kBatchSize));
  if (recs == nullptr) return BatchResult::Failed;

  const size_t n = storage::read_shots_from(hwm, recs, kBatchSize);
  if (n == 0) { std::free(recs); return BatchResult::UpToDate; }

  std::string payload;
  {
    auto* jbuf = static_cast<json::ShotJson*>(std::malloc(sizeof(json::ShotJson) * n));
    if (jbuf == nullptr) { std::free(recs); return BatchResult::Failed; }
    for (size_t i = 0; i < n; ++i) jbuf[i] = to_json(recs[i], hwm + static_cast<uint32_t>(i));
    payload = json::build_payload(token, jbuf, n);
    std::free(jbuf);
  }
  std::free(recs);

  RespCtx resp{};
  int http_status = 0;
  const esp_err_t err = http_post(url, payload, &http_status, &resp);

  // ContentService always answers 200, so "did it work?" is the body's ok flag,
  // not the status code — a bad token still returns 200 with ok:false.
  const bool ok = (err == ESP_OK && http_status == 200 &&
                   std::strstr(resp.buf, "\"ok\":true") != nullptr);
  if (!ok) {
    { Lock l; s_last_error = (err != ESP_OK) ? err : ESP_FAIL; }
    ESP_LOGW(kTag, "upload failed: err=%s http=%d resp=%.64s",
             esp_err_to_name(err), http_status, resp.buf);
    return BatchResult::Failed;
  }

  uint32_t new_hwm;
  {
    Lock l;
    s_hwm += static_cast<uint32_t>(n);
    new_hwm = s_hwm;
    s_last_error = ESP_OK;
  }
  persist_hwm(new_hwm);
  ESP_LOGI(kTag, "uploaded %u records, hwm=%u/%u",
           static_cast<unsigned>(n), static_cast<unsigned>(new_hwm),
           static_cast<unsigned>(total));
  return BatchResult::Ok;
}

// --- time sync (SNTP) ---

// Called from the SNTP task once a server reply lands. The module has already
// pushed the time into the system clock (settimeofday), so we just nudge the
// sync task to copy it into the battery-backed RTC — done there to keep the
// blocking I²C write off the lwip task.
void on_sntp_sync(struct timeval* /*tv*/) {
  if (s_events) xEventGroupSetBits(s_events, kBitTimeSynced);
}

// Bring up the SNTP client once, on the first GOT_IP. It then re-syncs itself
// periodically and across reconnects, so this need only run a single time. The
// build-time seed in rtc::init() is just a cold-boot floor; this is what makes
// the clock actually correct (and self-healing) whenever WiFi is up.
void start_sntp() {
  if (s_sntp_inited) return;
  esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  cfg.sync_cb = on_sntp_sync;
  if (esp_netif_sntp_init(&cfg) == ESP_OK) {
    s_sntp_inited = true;
    ESP_LOGI(kTag, "SNTP started (pool.ntp.org)");
  } else {
    ESP_LOGW(kTag, "SNTP init failed");
  }
}

void sync_task(void* /*arg*/) {
  ESP_LOGI(kTag, "sync task started (stack %u, core %d)",
           static_cast<unsigned>(kSyncTaskStack), static_cast<int>(kSyncTaskCore));
  uint32_t backoff = kBackoffMinMs;
  for (;;) {
    const EventBits_t bits =
        xEventGroupWaitBits(s_events,
                            kBitGotIp | kBitNewShot | kBitProvConnect | kBitTimeSynced,
                            /*clear=*/pdTRUE, /*waitAll=*/pdFALSE, portMAX_DELAY);
    if (bits & kBitTimeSynced) {
      // SNTP set the system clock; copy it into the RTC so the time survives
      // reboots and stamps shots correctly. Guard against a bogus pre-2024 value.
      struct timeval tv = {};
      gettimeofday(&tv, nullptr);
      if (tv.tv_sec > 1700000000) {
        const esp_err_t e = rtc::set_time(static_cast<uint32_t>(tv.tv_sec));
        ESP_LOGI(kTag, "RTC set from SNTP: epoch=%ld (%s)",
                 static_cast<long>(tv.tv_sec), esp_err_to_name(e));
      }
    }
    if (bits & kBitProvConnect) {
      // Finalize provisioning off the httpd task: a portal client (the phone)
      // still attached to the SoftAP pins the single radio to the AP channel and
      // blocks the STA from associating to a home AP on another channel — so drop
      // the AP first, then connect STA-only. The short delay lets the
      // "Connecting…" response flush to the phone before httpd stops.
      vTaskDelay(pdMS_TO_TICKS(600));
      portal::stop();
      esp_wifi_set_mode(WIFI_MODE_STA);
      { Lock l; s_wifi_retry = 0; }
      set_wifi_state(WifiState::Connecting);
      // If the STA is already associated (re-provisioning, or the same SSID as a
      // prior session), esp_wifi_connect() is a no-op and no fresh GOT_IP fires —
      // we'd hang in Connecting. Drop the link first; the STA_DISCONNECTED
      // handler (provisioning + submitted) then connects with the new creds.
      wifi_ap_record_t cur;
      if (esp_wifi_sta_get_ap_info(&cur) == ESP_OK) {
        esp_wifi_disconnect();
      } else {
        esp_wifi_connect();
      }
    }
    // Drain everything above the HWM, retrying with backoff on failure, until
    // we're up to date or WiFi/endpoint goes away (then back to waiting).
    for (;;) {
      if (!is_connected() || !configured()) { set_sync_state(SyncState::Idle); break; }
      set_sync_state(SyncState::Syncing);
      const BatchResult r = upload_batch();
      if (r == BatchResult::Ok) { backoff = kBackoffMinMs; continue; }
      if (r == BatchResult::UpToDate) {
        set_sync_state(SyncState::Idle);
        backoff = kBackoffMinMs;
        break;
      }
      set_sync_state(SyncState::Backoff);
      vTaskDelay(pdMS_TO_TICKS(backoff));
      backoff = backoff * kBackoffFactor;
      if (backoff > kBackoffMaxMs) backoff = kBackoffMaxMs;
    }
  }
}

// --- WiFi / provisioning events ---

void on_event(void* /*arg*/, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    // Fires once after esp_wifi_start(). During provisioning the manager owns
    // the connect sequence, so stay out of its way. Otherwise auto-connect only
    // if creds are stored; an unprovisioned device sits idle until Connect.
    { Lock l; if (s_provisioning) return; }
    if (has_stored_creds()) {
      set_wifi_state(WifiState::Connecting);
      esp_wifi_connect();
    } else {
      set_wifi_state(WifiState::Disabled);
    }
    return;
  }

  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    { Lock l; s_rssi = 0; s_net_ssid[0] = '\0'; }
    bool prov, submitted;
    int  retry;
    { Lock l; prov = s_provisioning; submitted = s_prov_submitted; retry = ++s_wifi_retry; }

    if (prov) {
      if (!submitted) return;  // AP up, no connect attempt yet — nothing to do
      if (retry <= kWifiMaxRetry) {
        // The first associate after the AP teardown often glitches; retry a few
        // times before concluding the password/SSID is wrong.
        esp_wifi_connect();
        return;
      }
      // Give up. Clear provisioning so the screen shows Failed and the Connect
      // button is live again (the portal/AP is already down by this point).
      ESP_LOGW(kTag, "provisioning: connect failed after %d tries (wrong password / AP?)",
               kWifiMaxRetry);
      portal::stop();
      esp_wifi_set_mode(WIFI_MODE_STA);
      { Lock l; s_provisioning = false; s_prov_submitted = false; s_prov_ssid[0] = '\0'; }
      set_wifi_state(WifiState::Failed);
      return;
    }

    // Creds were erased (Forget) — settle to idle instead of reconnecting.
    if (!has_stored_creds()) { set_wifi_state(WifiState::Disabled); return; }

    if (retry <= kWifiMaxRetry) {
      set_wifi_state(WifiState::Connecting);
      esp_wifi_connect();
    } else {
      set_wifi_state(WifiState::Failed);
      ESP_LOGW(kTag, "gave up reconnecting after %d attempts", kWifiMaxRetry);
    }
    return;
  }

  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    int8_t rssi = 0;
    char   ssid[33] = {};
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
      rssi = ap.rssi;
      std::memcpy(ssid, ap.ssid,
                  strnlen(reinterpret_cast<const char*>(ap.ssid), sizeof(ssid) - 1));
    }
    bool was_provisioning;
    {
      Lock l;
      s_wifi = WifiState::Connected;
      s_wifi_retry = 0;
      s_rssi = rssi;
      std::memcpy(s_net_ssid, ssid, sizeof(s_net_ssid));
      was_provisioning = s_provisioning;
    }
    ESP_LOGI(kTag, "connected, rssi=%d", rssi);
    start_sntp();  // correct the clock now that we're online (idempotent)
    if (was_provisioning) {
      // Provisioning succeeded. Tear the captive portal down here in the event
      // task (never from the httpd task that served the form — it'd kill itself)
      // and drop back to STA-only so the setup AP disappears.
      portal::stop();
      esp_wifi_set_mode(WIFI_MODE_STA);
      Lock l;
      s_provisioning = false;
      s_prov_submitted = false;
      s_prov_ssid[0] = '\0';
    }
    xEventGroupSetBits(s_events, kBitGotIp);
    return;
  }
}

// --- serial console ---

void print_status() {
  const Status st = status();
  const char* w = "?";
  switch (st.wifi) {
    case WifiState::Disabled:     w = "disabled"; break;
    case WifiState::Provisioning: w = "provisioning"; break;
    case WifiState::Connecting:   w = "connecting"; break;
    case WifiState::Connected:    w = "connected"; break;
    case WifiState::Failed:       w = "failed"; break;
  }
  const char* s = "?";
  switch (st.sync) {
    case SyncState::Idle:    s = "idle"; break;
    case SyncState::Syncing: s = "syncing"; break;
    case SyncState::Backoff: s = "backoff"; break;
    case SyncState::Error:   s = "error"; break;
  }
  size_t token_len;
  { Lock l; token_len = std::strlen(s_token); }
  std::printf("wifi=%s sync=%s synced=%u pending=%u rssi=%d configured=%s "
              "url=%s token=%s last_err=%s\n",
              w, s, static_cast<unsigned>(st.synced_count),
              static_cast<unsigned>(st.pending_count), st.rssi_dbm,
              st.configured ? "yes" : "no",
              s_url[0] ? s_url : "(unset)",
              token_len ? "(set)" : "(unset)",
              esp_err_to_name(st.last_error));
  if (st.wifi == WifiState::Provisioning && st.prov_ssid[0]) {
    std::printf("  provisioning: join open AP '%s', the setup page opens at "
                "http://192.168.4.1/\n", st.prov_ssid);
  }
}

int cmd_cloud(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: cloud <set-url URL | set-token TOKEN | status | provision | sync | resync>\n");
    return 0;
  }
  const char* sub = argv[1];

  if (std::strcmp(sub, "set-url") == 0) {
    if (argc < 3) { std::printf("usage: cloud set-url <https-url>\n"); return 1; }
    const char* url = argv[2];
    if (std::strncmp(url, "https://", 8) != 0) {
      std::printf("error: url must start with https://\n");
      return 1;
    }
    if (std::strlen(url) >= kUrlMax) {
      std::printf("error: url too long (max %u)\n", static_cast<unsigned>(kUrlMax - 1));
      return 1;
    }
    if (persist_str(kNvsKeyUrl, url, s_url, sizeof(s_url))) std::printf("url set\n");
    else std::printf("error: nvs write failed\n");
    return 0;
  }

  if (std::strcmp(sub, "set-token") == 0) {
    if (argc < 3) { std::printf("usage: cloud set-token <token>\n"); return 1; }
    const char* tok = argv[2];
    if (std::strlen(tok) >= kTokenMax) {
      std::printf("error: token too long (max %u)\n", static_cast<unsigned>(kTokenMax - 1));
      return 1;
    }
    // Never echo the token value — only its length.
    if (persist_str(kNvsKeyToken, tok, s_token, sizeof(s_token))) {
      std::printf("token set (%u chars)\n", static_cast<unsigned>(std::strlen(tok)));
    } else {
      std::printf("error: nvs write failed\n");
    }
    return 0;
  }

  if (std::strcmp(sub, "status") == 0) { print_status(); return 0; }

  if (std::strcmp(sub, "provision") == 0) {
    const esp_err_t e = start_provisioning();
    std::printf("provisioning: %s\n", esp_err_to_name(e));
    return 0;
  }

  if (std::strcmp(sub, "sync") == 0) {
    if (s_events) xEventGroupSetBits(s_events, kBitNewShot);
    std::printf("sync kicked\n");
    return 0;
  }

  // Rewind the high-water mark so every stored shot is re-uploaded from index 0.
  // Use after clearing/recreating the destination sheet — the device otherwise
  // only sends records above the HWM and would never re-offer the old ones. The
  // Apps Script dedupes by index, so this is safe even if some rows still exist.
  if (std::strcmp(sub, "resync") == 0) {
    { Lock l; s_hwm = 0; }
    persist_hwm(0);
    if (s_events) xEventGroupSetBits(s_events, kBitNewShot);
    std::printf("hwm reset to 0; re-uploading all records\n");
    return 0;
  }

  std::printf("unknown subcommand '%s'\n", sub);
  return 1;
}

void setup_console() {
  esp_console_repl_t* repl = nullptr;
  esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_cfg.prompt = "esp>";
  // The board's only USB port is the ESP32-S3 native USB-Serial-JTAG (no UART
  // bridge), so the REPL must read input from there — a UART0 REPL would print
  // the prompt (mirrored to the secondary console) but never see keystrokes.
  esp_console_dev_usb_serial_jtag_config_t hw_cfg =
      ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  if (esp_console_new_repl_usb_serial_jtag(&hw_cfg, &repl_cfg, &repl) != ESP_OK) {
    ESP_LOGW(kTag, "console repl init failed; cloud serial commands unavailable");
    return;
  }
  const esp_console_cmd_t cmd = {
      .command = "cloud",
      .help = "cloud sync: set-url/set-token/status/provision/sync/resync",
      .hint = nullptr,
      .func = &cmd_cloud,
  };
  esp_console_cmd_register(&cmd);
  esp_console_start_repl(repl);
}

}  // namespace

esp_err_t init() {
  if (s_task != nullptr) return ESP_ERR_INVALID_STATE;

  s_mutex = xSemaphoreCreateMutex();
  if (s_mutex == nullptr) return ESP_ERR_NO_MEM;
  s_events = xEventGroupCreate();
  if (s_events == nullptr) return ESP_ERR_NO_MEM;

  load_config();

  // We are the first/only owner of the default event loop + netif today; guard
  // creation against ESP_ERR_INVALID_STATE so a future second user is safe. Both
  // STA and AP netifs are needed: the captive portal brings up a temporary AP
  // during provisioning, STA is the normal connection.
  esp_err_t e = esp_netif_init();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
  e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
  s_netif = esp_netif_create_default_wifi_sta();
  esp_netif_create_default_wifi_ap();

  wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), kTag, "wifi_init");
  ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), kTag, "wifi_storage");

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr, nullptr);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr, nullptr);

  // "Already provisioned?" is just "are STA creds stored?" — esp_wifi_get_config
  // reads the FLASH-backed config and is valid after esp_wifi_init, before start.
  const bool provisioned = has_stored_creds();
  if (provisioned) {
    // Stored creds → normal STA bring-up; STA_START handler issues connect().
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "wifi_mode");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "wifi_start");
    { Lock l; s_started = true; }
    set_wifi_state(WifiState::Connecting);
  } else {
    // No creds: leave the radio idle until the user starts provisioning.
    set_wifi_state(WifiState::Disabled);
  }

  setup_console();

  xTaskCreatePinnedToCore(sync_task, "cloud", kSyncTaskStack, nullptr,
                          kSyncTaskPrio, &s_task, kSyncTaskCore);
  if (s_task == nullptr) return ESP_ERR_NO_MEM;

  ESP_LOGI(kTag, "init: %s, endpoint %s",
           provisioned ? "creds stored" : "no creds",
           configured() ? "set" : "unset");
  return ESP_OK;
}

namespace {
// Derive the SoftAP name from the WiFi MAC: deterministic (same code every time
// for a given device), unique per device, shown on the Connections screen as a
// Wi-Fi-join QR + text for the user to join.
void compute_prov_identity() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);  // reads efuse; works even pre-wifi-start
  Lock l;
  std::snprintf(s_prov_ssid, sizeof(s_prov_ssid), "EP SETUP %02X%02X%02X",
                mac[3], mac[4], mac[5]);
}

// Called from the httpd task when the captive-portal form is submitted. Persist
// the endpoint fields (a blank field means "keep the current value") and apply
// the Wi-Fi creds, then signal the sync task to do the actual AP-teardown +
// connect — we must NOT stop the portal or block from the httpd task it runs on.
void on_portal_submit(const portal::Submission& s) {
  if (s.url[0] != '\0')   persist_str(kNvsKeyUrl, s.url, s_url, sizeof(s_url));
  if (s.token[0] != '\0') persist_str(kNvsKeyToken, s.token, s_token, sizeof(s_token));

  // memcpy (not snprintf): the driver fields hold up to 32/64 bytes that need
  // not be NUL-terminated, and sta is zero-init so any unused tail stays NUL.
  wifi_config_t sta = {};
  std::memcpy(sta.sta.ssid, s.ssid, strnlen(s.ssid, sizeof(sta.sta.ssid)));
  std::memcpy(sta.sta.password, s.password,
              strnlen(s.password, sizeof(sta.sta.password)));
  esp_wifi_set_config(WIFI_IF_STA, &sta);  // persists to flash (storage = FLASH)

  { Lock l; s_prov_submitted = true; s_wifi_retry = 0; }
  set_wifi_state(WifiState::Connecting);
  ESP_LOGI(kTag, "provisioning: applying creds for '%s'", s.ssid);
  if (s_events) xEventGroupSetBits(s_events, kBitProvConnect);
}
}  // namespace

esp_err_t start_provisioning() {
  if (s_events == nullptr) return ESP_ERR_INVALID_STATE;
  { Lock l; if (s_provisioning) return ESP_OK; s_provisioning = true; s_prov_submitted = false; }

  compute_prov_identity();  // fills s_prov_ssid
  set_wifi_state(WifiState::Provisioning);

  // Bring the radio up in AP+STA: the SoftAP hosts the captive portal, and STA
  // must be running too so the form's network scan works and the submitted creds
  // can connect without another mode switch. STA is started once and left up.
  esp_err_t e = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (e == ESP_OK) {
    bool start_needed;
    { Lock l; start_needed = !s_started; }
    if (start_needed) {
      e = esp_wifi_start();
      if (e == ESP_OK) { Lock l; s_started = true; }
    }
  }

  // Prefill the form's URL field with the stored endpoint (never the token).
  { Lock l; portal::set_prefill(s_url); }

  if (e == ESP_OK) e = portal::start(s_prov_ssid, &on_portal_submit);

  if (e != ESP_OK) {
    portal::stop();
    { Lock l; s_provisioning = false; s_prov_submitted = false; s_prov_ssid[0] = '\0'; }
    set_wifi_state(has_stored_creds() ? WifiState::Connecting : WifiState::Disabled);
    ESP_LOGE(kTag, "provisioning start failed: %s", esp_err_to_name(e));
    return e;
  }
  ESP_LOGI(kTag, "captive-portal provisioning started: join '%s'", s_prov_ssid);
  return ESP_OK;
}

void cancel_provisioning() {
  { Lock l; if (!s_provisioning) return; s_provisioning = false; s_prov_submitted = false; }
  // Stop the portal (httpd + DNS) and drop the AP by returning to STA-only.
  portal::stop();
  esp_wifi_set_mode(WIFI_MODE_STA);
  { Lock l; s_prov_ssid[0] = '\0'; }
  // Ask the hardware, not s_wifi (which is Provisioning right now): provisioning
  // is non-destructive, so the STA is usually still associated to the prior
  // network and we just restore Connected. If the form's scan dropped the link,
  // reconnect; if there were never creds, go idle.
  wifi_ap_record_t cur;
  if (esp_wifi_sta_get_ap_info(&cur) == ESP_OK) {
    Lock l;
    s_wifi = WifiState::Connected;
    s_rssi = cur.rssi;
  } else if (has_stored_creds()) {
    { Lock l; s_wifi_retry = 0; }
    set_wifi_state(WifiState::Connecting);
    esp_wifi_connect();
  } else {
    set_wifi_state(WifiState::Disabled);
  }
}

void forget() {
  // Erase the stored STA creds first so the disconnect below settles to Disabled
  // (the STA_DISCONNECTED handler only reconnects while creds exist). The cloud
  // endpoint (NVS u/k) is intentionally left intact.
  wifi_config_t empty = {};
  esp_wifi_set_config(WIFI_IF_STA, &empty);
  esp_wifi_disconnect();
  { Lock l; s_wifi_retry = 0; s_rssi = 0; s_net_ssid[0] = '\0'; }
  set_wifi_state(WifiState::Disabled);
  ESP_LOGI(kTag, "forgot stored Wi-Fi credentials");
}

Status status() {
  Status st{};
  st.wifi = WifiState::Disabled;
  st.sync = SyncState::Idle;
  if (s_mutex == nullptr) return st;

  uint32_t hwm = 0;
  if (xSemaphoreTake(s_mutex, pdMS_TO_TICKS(5)) == pdTRUE) {
    st.wifi         = s_wifi;
    st.sync         = s_sync;
    st.synced_count = s_hwm;
    st.rssi_dbm     = s_rssi;
    st.last_error   = s_last_error;
    st.configured   = (s_url[0] != '\0' && s_token[0] != '\0');
    std::snprintf(st.net_ssid, sizeof(st.net_ssid), "%s", s_net_ssid);
    std::snprintf(st.prov_ssid, sizeof(st.prov_ssid), "%s", s_prov_ssid);
    hwm             = s_hwm;
    xSemaphoreGive(s_mutex);
  }
  // shot_count() takes storage's own lock — call it outside ours to avoid
  // holding two locks at once.
  const uint32_t total = storage::shot_count();
  st.pending_count = (total > hwm) ? (total - hwm) : 0;
  return st;
}

void notify_new_shot() {
  if (s_events != nullptr) xEventGroupSetBits(s_events, kBitNewShot);
}

}  // namespace espressopost::cloud
