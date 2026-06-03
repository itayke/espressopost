#include "cloud.hpp"

#include "cloud_json.hpp"
#include "storage.hpp"

#include "esp_check.h"
#include "esp_console.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
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
constexpr EventBits_t kBitGotIp   = 1u << 0;
constexpr EventBits_t kBitNewShot = 1u << 1;

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
bool      s_provisioning = false;

char s_url[kUrlMax]     = {};
char s_token[kTokenMax] = {};

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
};

esp_err_t http_event(esp_http_client_event_t* evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != nullptr) {
    auto* ctx = static_cast<RespCtx*>(evt->user_data);
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
// 302-redirects to script.googleusercontent.com; auto-redirect (default on)
// follows it as a GET to fetch the result — doPost already ran with the body on
// the first hop, so the body is delivered before the redirect. The cert bundle
// covers both hosts.
esp_err_t http_post(const char* url, const std::string& body, int* out_status, RespCtx* resp) {
  esp_http_client_config_t cfg = {};
  cfg.url               = url;
  cfg.method            = HTTP_METHOD_POST;
  cfg.timeout_ms        = kHttpTimeoutMs;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.event_handler     = http_event;
  cfg.user_data         = resp;

  esp_http_client_handle_t c = esp_http_client_init(&cfg);
  if (c == nullptr) return ESP_FAIL;

  esp_http_client_set_header(c, "Content-Type", "application/json");
  esp_http_client_set_post_field(c, body.data(), static_cast<int>(body.size()));

  const esp_err_t err = esp_http_client_perform(c);
  if (err == ESP_OK) *out_status = esp_http_client_get_status_code(c);
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

void sync_task(void* /*arg*/) {
  ESP_LOGI(kTag, "sync task started (stack %u, core %d)",
           static_cast<unsigned>(kSyncTaskStack), static_cast<int>(kSyncTaskCore));
  uint32_t backoff = kBackoffMinMs;
  for (;;) {
    xEventGroupWaitBits(s_events, kBitGotIp | kBitNewShot,
                        /*clear=*/pdTRUE, /*waitAll=*/pdFALSE, portMAX_DELAY);
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

// --- WiFi / SmartConfig events ---

void on_event(void* /*arg*/, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    // Fires once after esp_wifi_start(). Auto-connect only if creds are stored;
    // an unprovisioned device sits idle until start_provisioning().
    if (has_stored_creds()) {
      set_wifi_state(WifiState::Connecting);
      esp_wifi_connect();
    } else {
      set_wifi_state(WifiState::Disabled);
    }
    return;
  }

  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    { Lock l; s_rssi = 0; }
    bool prov; int retry;
    { Lock l; prov = s_provisioning; retry = ++s_wifi_retry; }
    if (prov) return;  // SmartConfig is mid-handshake; let it drive reconnect
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
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
    { Lock l; s_wifi = WifiState::Connected; s_wifi_retry = 0; s_rssi = rssi; }
    ESP_LOGI(kTag, "connected, rssi=%d", rssi);
    xEventGroupSetBits(s_events, kBitGotIp);
    return;
  }

  if (base == SC_EVENT && id == SC_EVENT_GOT_SSID_PSWD) {
    auto* evt = static_cast<smartconfig_event_got_ssid_pswd_t*>(data);
    wifi_config_t cfg = {};
    std::memcpy(cfg.sta.ssid, evt->ssid, sizeof(cfg.sta.ssid));
    std::memcpy(cfg.sta.password, evt->password, sizeof(cfg.sta.password));
    if (evt->bssid_set) {
      cfg.sta.bssid_set = true;
      std::memcpy(cfg.sta.bssid, evt->bssid, sizeof(cfg.sta.bssid));
    }
    ESP_LOGI(kTag, "got creds for SSID '%s' via SmartConfig", reinterpret_cast<char*>(cfg.sta.ssid));
    esp_wifi_disconnect();
    esp_wifi_set_config(WIFI_IF_STA, &cfg);
    set_wifi_state(WifiState::Connecting);
    { Lock l; s_wifi_retry = 0; }
    esp_wifi_connect();
    return;
  }

  if (base == SC_EVENT && id == SC_EVENT_SEND_ACK_DONE) {
    esp_smartconfig_stop();
    { Lock l; s_provisioning = false; }
    ESP_LOGI(kTag, "SmartConfig done");
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
}

int cmd_cloud(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: cloud <set-url URL | set-token TOKEN | status | provision | sync>\n");
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

  std::printf("unknown subcommand '%s'\n", sub);
  return 1;
}

void setup_console() {
  esp_console_repl_t* repl = nullptr;
  esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_cfg.prompt = "esp>";
  esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
  if (esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl) != ESP_OK) {
    ESP_LOGW(kTag, "console repl init failed; cloud serial commands unavailable");
    return;
  }
  const esp_console_cmd_t cmd = {
      .command = "cloud",
      .help = "cloud sync: set-url/set-token/status/provision/sync",
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
  // creation against ESP_ERR_INVALID_STATE so a future second user is safe.
  esp_err_t e = esp_netif_init();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
  e = esp_event_loop_create_default();
  if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) return e;
  s_netif = esp_netif_create_default_wifi_sta();

  wifi_init_config_t wcfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_RETURN_ON_ERROR(esp_wifi_init(&wcfg), kTag, "wifi_init");

  esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr, nullptr);
  esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_event, nullptr, nullptr);
  esp_event_handler_instance_register(SC_EVENT, ESP_EVENT_ANY_ID, &on_event, nullptr, nullptr);

  ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), kTag, "wifi_storage");
  ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), kTag, "wifi_mode");
  ESP_RETURN_ON_ERROR(esp_wifi_start(), kTag, "wifi_start");  // STA_START handler connects if creds stored

  setup_console();

  xTaskCreatePinnedToCore(sync_task, "cloud", kSyncTaskStack, nullptr,
                          kSyncTaskPrio, &s_task, kSyncTaskCore);
  if (s_task == nullptr) return ESP_ERR_NO_MEM;

  ESP_LOGI(kTag, "init: %s, endpoint %s",
           has_stored_creds() ? "creds stored" : "no creds",
           configured() ? "set" : "unset");
  return ESP_OK;
}

esp_err_t start_provisioning() {
  if (s_events == nullptr) return ESP_ERR_INVALID_STATE;
  { Lock l; if (s_provisioning) return ESP_OK; s_provisioning = true; }
  set_wifi_state(WifiState::Provisioning);

  ESP_RETURN_ON_ERROR(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH_V2), kTag, "sc_type");
  smartconfig_start_config_t cfg = SMARTCONFIG_START_CONFIG_DEFAULT();
  const esp_err_t e = esp_smartconfig_start(&cfg);
  if (e != ESP_OK) {
    { Lock l; s_provisioning = false; }
    set_wifi_state(has_stored_creds() ? WifiState::Connecting : WifiState::Disabled);
  }
  ESP_LOGI(kTag, "ESPTouch v2 provisioning started (%s)", esp_err_to_name(e));
  return e;
}

void cancel_provisioning() {
  { Lock l; if (!s_provisioning) return; s_provisioning = false; }
  esp_smartconfig_stop();
  set_wifi_state(is_connected() ? WifiState::Connected
                                : (has_stored_creds() ? WifiState::Connecting
                                                      : WifiState::Disabled));
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
