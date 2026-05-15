#pragma once

#include <cstdint>

#include "esp_err.h"

namespace espressopost::presets {

// Hard cap so on-screen selectors stay finite and so the NVS blob keys
// (`p0`..`p7`) never collide with anything else in the namespace.
constexpr uint8_t kMaxPresets = 8;
constexpr uint8_t kNameLen    = 16;   // including NUL terminator

// One brew preset. Persisted as a packed blob under NVS key `pN` so the
// on-disk shape matches the in-memory shape — no per-field serialization.
//
// `click_anchor` is the model's current grind-setting offset for this preset
// (signed clicks from baseline). It stays 0 until Step 5 (the model writes it).
struct __attribute__((packed)) Preset {
  char    name[kNameLen];
  uint8_t target_time_s;
  uint8_t dose_g;
  int8_t  click_anchor;
  uint8_t _pad;             // keep the blob a round 20 bytes
};
static_assert(sizeof(Preset) == 20, "Preset NVS blob size must be stable");

// Open the NVS namespace, seed the default preset table on first boot, and
// load the current selection. Safe to call once. Returns ESP_ERR_INVALID_STATE
// on a second call.
esp_err_t init();

// How many presets are currently configured (always >= 1 once `init()` has
// returned ESP_OK).
uint8_t count();

// Which preset is currently active (0..count()-1). Used by the Report screen
// and written into every ShotRecord.
uint8_t selected_id();

// Read-only view of a preset by id. Returns an empty Preset (all zeros) if
// `id >= count()`.
Preset get(uint8_t id);

// Cycle to the next preset, persist the selection to NVS, and return the new
// selected id. Wraps around at count(). Cheap; no measurable wear for daily use.
uint8_t cycle_selected();

}  // namespace espressopost::presets
