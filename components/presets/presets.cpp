#include "presets.hpp"

#include <cstring>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

namespace espressopost::presets {
namespace {

constexpr const char* kTag       = "presets";
constexpr const char* kNamespace = "presets";
constexpr const char* kKeyCount  = "count";
constexpr const char* kKeySel    = "selected";

bool    s_inited      = false;
uint8_t s_count       = 0;
uint8_t s_selected    = 0;
Preset  s_table[kMaxPresets] = {};

// Default presets seeded on first boot — picked to cover the common single-
// basket pulls. The user can curate this list once the Preset editor screen
// lands; until then these are good enough for daily use.
constexpr Preset kDefaults[] = {
    {"espresso",  30, 18, 0, 0},
    {"lungo",     40, 18, 0, 0},
    {"ristretto", 22, 18, 0, 0},
};
constexpr uint8_t kDefaultCount = sizeof(kDefaults) / sizeof(kDefaults[0]);
static_assert(kDefaultCount <= kMaxPresets, "default table exceeds cap");

// NVS keys are limited to 15 chars. `pN` (2 chars) leaves plenty of headroom
// even at kMaxPresets = 8.
void preset_key(uint8_t i, char out[4]) {
  out[0] = 'p';
  out[1] = static_cast<char>('0' + i);
  out[2] = '\0';
  out[3] = '\0';
}

esp_err_t seed_defaults(nvs_handle_t h) {
  s_count = kDefaultCount;
  for (uint8_t i = 0; i < kDefaultCount; ++i) {
    s_table[i] = kDefaults[i];
    char key[4];
    preset_key(i, key);
    const esp_err_t err = nvs_set_blob(h, key, &s_table[i], sizeof(Preset));
    if (err != ESP_OK) return err;
  }
  esp_err_t err = nvs_set_u8(h, kKeyCount, s_count);
  if (err != ESP_OK) return err;
  s_selected = 0;
  err = nvs_set_u8(h, kKeySel, s_selected);
  if (err != ESP_OK) return err;
  return nvs_commit(h);
}

esp_err_t load_table(nvs_handle_t h) {
  uint8_t n = 0;
  esp_err_t err = nvs_get_u8(h, kKeyCount, &n);
  if (err != ESP_OK || n == 0) return ESP_ERR_NVS_NOT_FOUND;
  if (n > kMaxPresets) n = kMaxPresets;
  s_count = n;
  for (uint8_t i = 0; i < n; ++i) {
    char key[4];
    preset_key(i, key);
    size_t sz = sizeof(Preset);
    err = nvs_get_blob(h, key, &s_table[i], &sz);
    if (err != ESP_OK || sz != sizeof(Preset)) {
      ESP_LOGW(kTag, "preset %u missing or wrong size — reseeding", i);
      return ESP_ERR_NVS_NOT_FOUND;
    }
  }
  uint8_t sel = 0;
  err = nvs_get_u8(h, kKeySel, &sel);
  if (err == ESP_ERR_NVS_NOT_FOUND) sel = 0;
  else if (err != ESP_OK)           return err;
  if (sel >= s_count) sel = 0;
  s_selected = sel;
  return ESP_OK;
}

}  // namespace

esp_err_t init() {
  if (s_inited) return ESP_ERR_INVALID_STATE;

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(kTag, "nvs partition needs erase (%s) — reformatting",
             esp_err_to_name(err));
    nvs_flash_erase();
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "nvs_flash_init failed: %s", esp_err_to_name(err));
    return err;
  }

  nvs_handle_t h;
  err = nvs_open(kNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "nvs_open '%s' failed: %s", kNamespace, esp_err_to_name(err));
    return err;
  }

  err = load_table(h);
  if (err == ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGI(kTag, "no preset table found — seeding defaults");
    err = seed_defaults(h);
  }
  nvs_close(h);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "preset load/seed failed: %s", esp_err_to_name(err));
    return err;
  }

  s_inited = true;
  ESP_LOGI(kTag, "%u presets loaded, selected=%u (\"%s\")",
           s_count, s_selected, s_table[s_selected].name);
  return ESP_OK;
}

uint8_t count() { return s_count; }

uint8_t selected_id() { return s_selected; }

Preset get(uint8_t id) {
  if (id >= s_count) return Preset{};
  return s_table[id];
}

uint8_t cycle_selected() {
  if (s_count == 0) return 0;
  s_selected = static_cast<uint8_t>((s_selected + 1) % s_count);

  nvs_handle_t h;
  if (nvs_open(kNamespace, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, kKeySel, s_selected);
    nvs_commit(h);
    nvs_close(h);
  }
  return s_selected;
}

}  // namespace espressopost::presets
