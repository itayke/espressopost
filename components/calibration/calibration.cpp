#include "calibration.hpp"

#include "esp_log.h"
#include "nvs.h"

#include <cstring>

namespace espressopost::calibration {
namespace {

constexpr const char* kTag = "calib";

// NVS (namespace "calib"): one blob holding the boundary array, length implied
// by blob size. Single-letter key matches the climate/cloud convention.
constexpr const char* kNvsNamespace = "calib";
constexpr const char* kNvsKeyList   = "b";

// RAM mirror of the persisted list, kept sorted ascending by rtc_epoch_s.
Boundary s_list[kMaxBoundaries] = {};
size_t   s_count                = 0;
bool     s_inited               = false;

// Insertion sort by rtc_epoch_s — n is tiny (≤ kMaxBoundaries) and we sort only
// on mutation, so the simplest stable thing is the right thing.
void sort_list() {
  for (size_t i = 1; i < s_count; ++i) {
    Boundary key = s_list[i];
    size_t j = i;
    while (j > 0 && s_list[j - 1].rtc_epoch_s > key.rtc_epoch_s) {
      s_list[j] = s_list[j - 1];
      --j;
    }
    s_list[j] = key;
  }
}

// Write the current RAM list back to NVS. An empty list erases the key so a
// fresh device and a cleared-out one read identically.
esp_err_t persist() {
  nvs_handle_t h;
  esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  if (s_count == 0) {
    err = nvs_erase_key(h, kNvsKeyList);
    if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;  // already absent
  } else {
    err = nvs_set_blob(h, kNvsKeyList, s_list, s_count * sizeof(Boundary));
  }
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  return err;
}

}  // namespace

esp_err_t init() {
  if (s_inited) return ESP_ERR_INVALID_STATE;
  s_inited = true;
  s_count  = 0;

  nvs_handle_t h;
  if (nvs_open(kNvsNamespace, NVS_READONLY, &h) != ESP_OK) {
    // No namespace yet — fresh device, empty list is correct.
    return ESP_OK;
  }
  // Ask NVS the blob size first (presets-style), so a partial/garbage blob can't
  // overrun the buffer. Anything that doesn't divide evenly into whole
  // Boundaries, or overflows the cap, is treated as "no usable list".
  size_t sz = 0;
  esp_err_t err = nvs_get_blob(h, kNvsKeyList, nullptr, &sz);
  if (err == ESP_OK && sz > 0 && sz % sizeof(Boundary) == 0 &&
      sz <= sizeof(s_list)) {
    err = nvs_get_blob(h, kNvsKeyList, s_list, &sz);
    if (err == ESP_OK) s_count = sz / sizeof(Boundary);
  } else if (err != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGW(kTag, "boundary blob unreadable (%s); starting empty",
             esp_err_to_name(err));
  }
  nvs_close(h);
  sort_list();
  ESP_LOGI(kTag, "loaded %u boundaries (%u epochs)", (unsigned)s_count,
           (unsigned)epoch_count());
  return ESP_OK;
}

size_t count() { return s_count; }

size_t list(Boundary* out, size_t max) {
  const size_t n = s_count < max ? s_count : max;
  std::memcpy(out, s_list, n * sizeof(Boundary));
  return n;
}

esp_err_t add(uint32_t rtc_epoch_s, Kind kind) {
  if (rtc_epoch_s == 0) return ESP_ERR_INVALID_ARG;
  if (s_count >= kMaxBoundaries) return ESP_ERR_NO_MEM;
  s_list[s_count++] = Boundary{rtc_epoch_s, static_cast<uint8_t>(kind)};
  sort_list();
  const esp_err_t err = persist();
  if (err != ESP_OK) {
    // Roll the RAM list back so it can't diverge from flash on a failed write.
    --s_count;
    sort_list();
  }
  return err;
}

esp_err_t remove_at(size_t i) {
  if (i >= s_count) return ESP_ERR_INVALID_ARG;
  for (size_t j = i; j + 1 < s_count; ++j) s_list[j] = s_list[j + 1];
  --s_count;
  return persist();
}

uint8_t epoch_index(uint32_t shot_rtc_epoch_s) {
  if (shot_rtc_epoch_s == 0) return 0;  // no timeline position → oldest epoch
  uint8_t idx = 0;
  for (size_t i = 0; i < s_count; ++i) {
    if (!is_epoch(static_cast<Kind>(s_list[i].kind))) continue;
    if (shot_rtc_epoch_s >= s_list[i].rtc_epoch_s && idx < 255) ++idx;
  }
  return idx;
}

uint8_t epoch_count() {
  uint8_t epochs = 1;
  for (size_t i = 0; i < s_count; ++i) {
    if (is_epoch(static_cast<Kind>(s_list[i].kind)) && epochs < 255) ++epochs;
  }
  return epochs;
}

}  // namespace espressopost::calibration
