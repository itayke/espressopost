#include "cloud_portal.hpp"

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "dhcpserver/dhcpserver.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace espressopost::cloud::portal {
namespace {

constexpr const char* kTag = "cloud.portal";

// --- Tunables ---
constexpr int      kDnsPort       = 53;
constexpr int      kDnsMaxLen     = 256;   // DNS-over-UDP messages are small
constexpr uint32_t kDnsTtlSec     = 300;
constexpr uint16_t kDnsTypeA      = 1;     // A (host address) query/answer
constexpr size_t   kMaxScan       = 16;    // SSIDs rendered in the dropdown
constexpr uint32_t kHttpStack     = 8192;  // GET handler runs a blocking scan
constexpr size_t   kBodyMax       = 1024;  // POST /save body cap

// --- State ---
SubmitCb          s_cb       = nullptr;
httpd_handle_t    s_httpd    = nullptr;
int               s_dns_sock = -1;
volatile bool     s_dns_run  = false;
uint32_t          s_ap_ip    = 0;          // network byte order, for DNS answers

wifi_ap_record_t  s_scan[kMaxScan];
size_t            s_scan_n   = 0;

// The endpoint URL prefilled into the form (not secret). The token is never
// echoed back, so it stays out of here. cloud.cpp passes the current URL in via
// set_prefill() before start(); empty when unset.
char              s_prefill_url[160] = {};

// ===== DNS responder ========================================================
// A minimal authoritative-looking server: answer every A query with our AP IP so
// the phone OS's captive-portal probe (and any name the browser resolves) lands
// on our HTTP server. Adapted from ESP-IDF's captive_portal example.

struct __attribute__((packed)) DnsHeader {
  uint16_t id, flags, qd_count, an_count, ns_count, ar_count;
};
struct __attribute__((packed)) DnsQuestion {
  uint16_t type, qclass;  // 'class' is reserved in C++
};
struct __attribute__((packed)) DnsAnswer {
  uint16_t ptr_offset, type, aclass;
  uint32_t ttl;
  uint16_t addr_len;
  uint32_t ip_addr;
};

// Walk a length-prefixed DNS name (e.g. 3www6google3com0) past its terminator,
// returning the pointer just after it, or nullptr if it runs off the buffer.
const char* skip_dns_name(const char* label, const char* end) {
  while (label < end && *label != 0) {
    int len = static_cast<uint8_t>(*label);
    label += len + 1;
  }
  return (label < end) ? label + 1 : nullptr;
}

// Build a reply into `out` (>= req_len + answers). Returns reply length, or -1.
int build_dns_reply(const char* req, size_t req_len, char* out, size_t out_max) {
  if (req_len < sizeof(DnsHeader) || req_len > out_max) return -1;
  std::memcpy(out, req, req_len);
  auto* h = reinterpret_cast<DnsHeader*>(out);
  if (ntohs(h->flags) & 0x8000) return -1;  // already a response — ignore
  const uint16_t qd = ntohs(h->qd_count);
  if (qd == 0) return -1;

  const size_t reply_len = req_len + qd * sizeof(DnsAnswer);
  if (reply_len > out_max) return -1;
  h->flags |= htons(0x8000);  // QR = response
  h->an_count = h->qd_count;  // one A answer per question

  const char* end = out + req_len;
  const char* qd_ptr = out + sizeof(DnsHeader);
  char* ans_ptr = out + req_len;
  for (uint16_t i = 0; i < qd; ++i) {
    const char* after_name = skip_dns_name(qd_ptr, end);
    if (after_name == nullptr || after_name + sizeof(DnsQuestion) > end) return -1;
    qd_ptr = after_name + sizeof(DnsQuestion);

    auto* a = reinterpret_cast<DnsAnswer*>(ans_ptr);
    a->ptr_offset = htons(0xC000 | sizeof(DnsHeader));  // points at the 1st question
    a->type       = htons(kDnsTypeA);
    a->aclass     = htons(1);   // IN
    a->ttl        = htonl(kDnsTtlSec);
    a->addr_len   = htons(4);
    a->ip_addr    = s_ap_ip;    // already network order
    ans_ptr += sizeof(DnsAnswer);
  }
  return static_cast<int>(reply_len);
}

void dns_task(void* /*arg*/) {
  char rx[kDnsMaxLen];
  char tx[kDnsMaxLen + 64];  // request copy + appended A answers (16 B each)
  while (s_dns_run) {
    struct sockaddr_in client = {};
    socklen_t slen = sizeof(client);
    const int len = recvfrom(s_dns_sock, rx, sizeof(rx), 0,
                             reinterpret_cast<struct sockaddr*>(&client), &slen);
    if (len < 0) break;  // socket shut down by stop()
    const int reply = build_dns_reply(rx, static_cast<size_t>(len), tx, sizeof(tx));
    if (reply > 0) {
      sendto(s_dns_sock, tx, reply, 0,
             reinterpret_cast<struct sockaddr*>(&client), slen);
    }
  }
  s_dns_sock = -1;
  vTaskDelete(nullptr);
}

esp_err_t start_dns() {
  s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (s_dns_sock < 0) return ESP_FAIL;
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  addr.sin_port = htons(kDnsPort);
  if (bind(s_dns_sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(s_dns_sock);
    s_dns_sock = -1;
    return ESP_FAIL;
  }
  s_dns_run = true;
  xTaskCreatePinnedToCore(dns_task, "captdns", 3072, nullptr, 4, nullptr, 0);
  return ESP_OK;
}

// ===== HTTP form ============================================================

// Append `in` to `out` with the five HTML/attribute-significant characters
// escaped, so a network name or URL with quotes/angle-brackets can't break the
// markup (or inject).
void append_escaped(std::string& out, const char* in) {
  for (const char* p = in; *p; ++p) {
    switch (*p) {
      case '&': out += "&amp;";  break;
      case '<': out += "&lt;";   break;
      case '>': out += "&gt;";   break;
      case '"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default:  out += *p;       break;
    }
  }
}

// Blocking scan of nearby networks, cached after the first success. Runs in the
// httpd task (never the LVGL task), so the ~1.5 s block is invisible to the UI.
void ensure_scan() {
  if (s_scan_n > 0) return;
  wifi_scan_config_t sc = {};  // active scan, all channels
  if (esp_wifi_scan_start(&sc, /*block=*/true) != ESP_OK) return;
  uint16_t n = kMaxScan;
  if (esp_wifi_scan_get_ap_records(&n, s_scan) != ESP_OK) { n = 0; }
  s_scan_n = (n > kMaxScan) ? kMaxScan : n;
}

std::string build_form_html() {
  ensure_scan();
  std::string h;
  h.reserve(2048);
  h += "<!DOCTYPE html><html><head><meta charset=utf-8>"
       "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
       "<title>Espresso Post Setup</title><style>"
       "body{font-family:system-ui,-apple-system,sans-serif;margin:0;background:#111;color:#eee}"
       ".c{max-width:430px;margin:0 auto;padding:24px 20px}"
       "h1{font-size:20px;font-weight:600}"
       "label{display:block;margin:16px 0 5px;font-size:14px;color:#bbb}"
       "input,select{width:100%;box-sizing:border-box;padding:11px;border-radius:8px;"
       "border:1px solid #444;background:#1c1c1c;color:#eee;font-size:16px}"
       // gap so the network dropdown's bottom border doesn't collide with the
       // hidden-network field stacked directly beneath it
       "select{margin-bottom:8px}"
       "button{margin-top:22px;width:100%;padding:13px;border:0;border-radius:24px;"
       "background:#C88036;color:#111;font-size:16px;font-weight:600}"
       "small{color:#888;font-weight:400}</style></head><body><div class=c>"
       "<h1>\xE2\x98\x95 Espresso Post Setup</h1>"
       "<form method=POST action=/save>"
       "<label>Wi-Fi network</label>"
       // A real <select> renders a native dropdown everywhere (unlike <datalist>,
       // which mobile browsers show inconsistently). The manual field below
       // covers hidden networks; on submit it wins when non-empty.
       "<select name=ssid><option value=\"\" selected>\xE2\x80\x94 pick a network \xE2\x80\x94</option>";
  // De-duplicate SSIDs (multiple APs / bands share a name) and skip empties.
  for (size_t i = 0; i < s_scan_n; ++i) {
    const char* ssid = reinterpret_cast<const char*>(s_scan[i].ssid);
    if (ssid[0] == '\0') continue;
    bool dup = false;
    for (size_t j = 0; j < i; ++j) {
      if (std::strcmp(ssid, reinterpret_cast<const char*>(s_scan[j].ssid)) == 0) {
        dup = true;
        break;
      }
    }
    if (dup) continue;
    h += "<option value=\"";
    append_escaped(h, ssid);
    h += "\">";
    append_escaped(h, ssid);
    h += "</option>";
  }
  h += "</select>"
       "<input name=ssidm autocomplete=off placeholder=\"\xE2\x80\xA6or type a hidden network\">"
       "<label>Password</label>"
       "<input name=pass type=password autocomplete=off>"
       "<label>Cloud endpoint URL</label>"
       "<input name=url type=url value=\"";
  append_escaped(h, s_prefill_url);
  h += "\" placeholder=\"https://script.google.com/.../exec\">"
       "<label>Token <small>(leave blank to keep current)</small></label>"
       "<input name=token autocomplete=off>"
       "<button type=submit>Connect</button>"
       "</form></div></body></html>";
  return h;
}

esp_err_t root_get(httpd_req_t* req) {
  const std::string html = build_form_html();
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html.data(), static_cast<ssize_t>(html.size()));
}

// Percent-decode `in` (also '+' -> space) into NUL-terminated `out`.
size_t url_decode(const char* in, size_t in_len, char* out, size_t out_max) {
  auto hex = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    c |= 0x20;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
  };
  size_t o = 0;
  for (size_t i = 0; i < in_len && o + 1 < out_max; ++i) {
    char c = in[i];
    if (c == '+') {
      c = ' ';
    } else if (c == '%' && i + 2 < in_len) {
      const int hi = hex(in[i + 1]), lo = hex(in[i + 2]);
      if (hi >= 0 && lo >= 0) { c = static_cast<char>(hi * 16 + lo); i += 2; }
    }
    out[o++] = c;
  }
  out[o] = '\0';
  return o;
}

// Copy a decoded value into a fixed-size field, truncating to fit and always
// NUL-terminating. memcpy rather than snprintf so the compiler's
// format-truncation check (the fields are smaller than the decode scratch) is
// moot — truncation here is intended; validation happens after the parse.
void set_field(char* dst, size_t cap, const char* src) {
  const size_t n = strnlen(src, cap - 1);
  std::memcpy(dst, src, n);
  dst[n] = '\0';
}

// Parse an application/x-www-form-urlencoded body into the Submission fields.
// Keys arrive un-encoded here (they're our own field names); values are decoded.
void parse_form(const char* body, size_t len, Submission& s) {
  s = {};
  size_t i = 0;
  while (i < len) {
    const size_t ks = i;
    while (i < len && body[i] != '=' && body[i] != '&') ++i;
    const size_t kl = i - ks;
    char val[256] = {};
    if (i < len && body[i] == '=') {
      ++i;
      const size_t vs = i;
      while (i < len && body[i] != '&') ++i;
      url_decode(body + vs, i - vs, val, sizeof(val));
    }
    if (i < len && body[i] == '&') ++i;
    auto key_is = [&](const char* k) {
      return std::strlen(k) == kl && std::memcmp(body + ks, k, kl) == 0;
    };
    if      (key_is("ssid"))  set_field(s.ssid,     sizeof(s.ssid),     val);
    // Manual entry (hidden networks) overrides the dropdown when non-empty.
    else if (key_is("ssidm")) { if (val[0]) set_field(s.ssid, sizeof(s.ssid), val); }
    else if (key_is("pass"))  set_field(s.password, sizeof(s.password), val);
    else if (key_is("url"))   set_field(s.url,      sizeof(s.url),      val);
    else if (key_is("token")) set_field(s.token,    sizeof(s.token),    val);
  }
}

esp_err_t send_message(httpd_req_t* req, const char* title, const char* body) {
  std::string h =
      "<!DOCTYPE html><html><head><meta charset=utf-8>"
      "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
      "<style>body{font-family:system-ui,sans-serif;background:#111;color:#eee;"
      "margin:0}.c{max-width:430px;margin:0 auto;padding:40px 24px;text-align:center}"
      "h1{font-size:20px}p{color:#bbb;line-height:1.5}</style></head>"
      "<body><div class=c><h1>";
  append_escaped(h, title);
  h += "</h1><p>";
  append_escaped(h, body);
  h += "</p></div></body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, h.data(), static_cast<ssize_t>(h.size()));
}

esp_err_t save_post(httpd_req_t* req) {
  const int total = req->content_len;
  if (total <= 0 || static_cast<size_t>(total) > kBodyMax) {
    return send_message(req, "Setup error", "The form was too large. Go back and retry.");
  }
  char body[kBodyMax + 1];
  int got = 0;
  while (got < total) {
    const int r = httpd_req_recv(req, body + got, total - got);
    if (r <= 0) return send_message(req, "Setup error", "Upload interrupted. Go back and retry.");
    got += r;
  }
  body[got] = '\0';

  Submission sub;
  parse_form(body, static_cast<size_t>(got), sub);

  if (sub.ssid[0] == '\0') {
    return send_message(req, "Setup error", "Please choose a Wi-Fi network.");
  }
  if (sub.url[0] != '\0' && std::strncmp(sub.url, "https://", 8) != 0) {
    return send_message(req, "Setup error", "The endpoint URL must start with https://");
  }

  // Hand the fields to cloud.cpp (persist + connect) before responding, so the
  // STA connect is already under way as the phone renders the confirmation.
  if (s_cb) s_cb(sub);

  char msg[160];  // ~78 chars of fixed text + up to a 32-char SSID
  std::snprintf(msg, sizeof(msg),
                "Connecting to \"%s\". This setup network will disappear once the "
                "device is online.", sub.ssid);
  return send_message(req, "Connecting\xE2\x80\xA6", msg);
}

}  // namespace

void set_prefill(const char* url) {
  std::snprintf(s_prefill_url, sizeof(s_prefill_url), "%s", url ? url : "");
}

esp_err_t start(const char* ap_ssid, SubmitCb cb) {
  s_cb = cb;
  s_scan_n = 0;  // force a fresh scan for this session

  // --- SoftAP config (open network; the security tradeoff is accepted) ---
  wifi_config_t ap = {};
  const size_t n = std::strlen(ap_ssid);
  std::memcpy(ap.ap.ssid, ap_ssid, n > 32 ? 32 : n);
  ap.ap.ssid_len      = static_cast<uint8_t>(n > 32 ? 32 : n);
  ap.ap.authmode      = WIFI_AUTH_OPEN;
  ap.ap.max_connection = 4;
  ap.ap.channel       = 1;
  ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), kTag, "ap_config");

  // --- Make the AP's DHCP hand out itself as the DNS server, so every lookup
  // reaches our responder. Must be set with dhcps stopped. ---
  esp_netif_t* ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
  if (ap_netif != nullptr) {
    esp_netif_ip_info_t ip_info = {};
    esp_netif_get_ip_info(ap_netif, &ip_info);
    s_ap_ip = ip_info.ip.addr;  // network byte order

    esp_netif_dhcps_stop(ap_netif);
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4.addr = ip_info.ip.addr;
    esp_netif_set_dns_info(ap_netif, ESP_NETIF_DNS_MAIN, &dns);
    dhcps_offer_t offer = OFFER_DNS;
    esp_netif_dhcps_option(ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &offer, sizeof(offer));
    esp_netif_dhcps_start(ap_netif);
  }

  // --- DNS responder ---
  if (start_dns() != ESP_OK) {
    ESP_LOGW(kTag, "DNS responder failed to start; captive auto-pop may not work");
  }

  // --- HTTP server: wildcard GET serves the form (which is what makes the OS
  // captive check fail and pop the sheet); POST /save applies it. ---
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.stack_size      = kHttpStack;
  cfg.lru_purge_enable = true;
  cfg.uri_match_fn    = httpd_uri_match_wildcard;
  if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
    ESP_LOGE(kTag, "httpd_start failed");
    stop();
    return ESP_FAIL;
  }
  const httpd_uri_t save_uri = {
      .uri = "/save", .method = HTTP_POST, .handler = save_post, .user_ctx = nullptr};
  const httpd_uri_t root_uri = {
      .uri = "/*", .method = HTTP_GET, .handler = root_get, .user_ctx = nullptr};
  httpd_register_uri_handler(s_httpd, &save_uri);
  httpd_register_uri_handler(s_httpd, &root_uri);

  ESP_LOGI(kTag, "captive portal up: AP '%s', form at http://192.168.4.1/", ap_ssid);
  return ESP_OK;
}

void stop() {
  if (s_httpd != nullptr) {
    httpd_stop(s_httpd);
    s_httpd = nullptr;
  }
  if (s_dns_run) {
    s_dns_run = false;
    if (s_dns_sock >= 0) {
      shutdown(s_dns_sock, SHUT_RDWR);  // unblock recvfrom so the task exits
      close(s_dns_sock);
    }
  }
  s_cb = nullptr;
}

}  // namespace espressopost::cloud::portal
