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

bool    s_inited   = false;
uint8_t s_selected = 0;
Preset  s_table[kMaxPresets] = {};

constexpr uint8_t kPresetVersion = 4;

// Default starting grind for seeded presets. Picked to match the historical
// state the user has been grinding at on this device; future builds with
// no migration to do start at the same value so the Report UI's auto-fill
// is immediately useful.
constexpr float kDefaultGrindAnchor = 5.2f;

// Default per-preset accent — light grey, matching the UI's base text tier so
// seeded/migrated presets read the same as before color existed. Kept off
// max-intensity white on purpose (AMOLED burn-in). The Presets screen will let
// the user override this per slot later.
constexpr uint32_t kDefaultPresetColor = 0xE0E0E0u;

// Default presets seeded on first boot — picked to cover the common single-
// basket pulls. The user can curate this list once the Preset editor screen
// lands; until then these are good enough for daily use. Yields are seeded
// at the same classic espresso ratio used for the v2→v3 backfill below.
constexpr Preset kDefaults[] = {
    {kPresetVersion, 30, 17, 34, "PRESET 1", kDefaultGrindAnchor, kDefaultPresetColor},
    {kPresetVersion, 40, 18, 36, "PRESET 2", kDefaultGrindAnchor, kDefaultPresetColor},
    {kPresetVersion, 22, 18, 36, "PRESET 3", kDefaultGrindAnchor, kDefaultPresetColor},
};
constexpr uint8_t kDefaultCount = sizeof(kDefaults) / sizeof(kDefaults[0]);
static_assert(kDefaultCount <= kMaxPresets, "default table exceeds cap");

// v1 on-disk layout, retained only for the one-shot migration in load_table().
// Production code reads/writes the current `Preset` from the header.
struct __attribute__((packed)) PresetV1 {
  char    name[kNameLen];
  uint8_t target_time_s;
  uint8_t dose_g;
  int8_t  click_anchor;
  uint8_t _pad;
};
static_assert(sizeof(PresetV1) == 20, "v1 preset size must match the schema we migrated away from");

// Pre-color (v2/v3) on-disk layout — the 24-byte blob before `color` was
// appended. Retained only for the one-shot v3→v4 migration in load_one_preset.
// Same field order as the current Preset, minus the trailing color word.
struct __attribute__((packed)) PresetV3 {
  uint8_t version;
  uint8_t target_time_s;
  uint8_t dose_g;
  uint8_t yield_g;
  char    name[kNameLen];
  float   grind_anchor;
};
static_assert(sizeof(PresetV3) == 24, "pre-color preset size must match the schema we migrated away from");

// NVS keys are limited to 15 chars. `pN` (2 chars) leaves plenty of headroom
// even at kMaxPresets = 9.
void preset_key(uint8_t i, char out[4]) {
  out[0] = 'p';
  out[1] = static_cast<char>('0' + i);
  out[2] = '\0';
  out[3] = '\0';
}

// Last-grind keys live in the same namespace under `gN`. Stored as a u32 bit
// pattern of the float so we can round-trip through nvs_get_u32 — NVS lacks
// a native float type, and u32 is cheaper than a 4-byte blob.
void grind_key(uint8_t i, char out[4]) {
  out[0] = 'g';
  out[1] = static_cast<char>('0' + i);
  out[2] = '\0';
  out[3] = '\0';
}

esp_err_t seed_defaults(nvs_handle_t h) {
  // Slots beyond kDefaultCount stay inactive (no NVS blob). kKeyCount is
  // now only a "table has been initialized" sentinel — its presence (not
  // its value) is what gates the seed path; load_table no longer reads
  // its value to decide loop bounds.
  for (uint8_t i = 0; i < kDefaultCount; ++i) {
    s_table[i] = kDefaults[i];
    char key[4];
    preset_key(i, key);
    const esp_err_t err = nvs_set_blob(h, key, &s_table[i], sizeof(Preset));
    if (err != ESP_OK) return err;
  }
  esp_err_t err = nvs_set_u8(h, kKeyCount, kDefaultCount);
  if (err != ESP_OK) return err;
  s_selected = 0;
  err = nvs_set_u8(h, kKeySel, s_selected);
  if (err != ESP_OK) return err;
  return nvs_commit(h);
}

// Read one preset blob and bring it up to the current schema in place. Three
// migration paths are supported, all keyed off the on-disk blob size: v1 (20 B)
// → current (full rewrite), and v2/v3 (24 B, pre-color) → v4 (appends `color`).
// The 24-byte path also still backfills yield_g from dose_g for any blob that
// predates v3 (version byte < 3). On any migration the rewritten blob is
// written straight back to NVS so the next boot is a fast current-version read.
esp_err_t load_one_preset(nvs_handle_t h, uint8_t i, bool* out_migrated) {
  char key[4];
  preset_key(i, key);

  // Ask NVS the actual blob size first — handles every layout without a
  // guess-and-retry dance.
  size_t sz = 0;
  esp_err_t err = nvs_get_blob(h, key, nullptr, &sz);
  if (err != ESP_OK) return err;

  if (sz == sizeof(Preset)) {
    // Current 28-byte v4 layout — straight read, no migration.
    err = nvs_get_blob(h, key, &s_table[i], &sz);
    if (err != ESP_OK) return err;
    return ESP_OK;
  }

  if (sz == sizeof(PresetV3)) {
    // Pre-color 24-byte blob (v2 or v3). Lift the carried fields, backfill
    // yield for anything older than v3, and seed the new `color` word.
    PresetV3 old_blob = {};
    err = nvs_get_blob(h, key, &old_blob, &sz);
    if (err != ESP_OK) return err;

    Preset& dst = s_table[i];
    dst = {};
    dst.version       = kPresetVersion;
    dst.target_time_s = old_blob.target_time_s;
    dst.dose_g        = old_blob.dose_g;
    // v2 stored `yield_g` as `_pad` (always zero) — backfill from dose at the
    // default espresso ratio; v3 already carries a real yield.
    dst.yield_g       = old_blob.version < 3
                            ? static_cast<uint8_t>(old_blob.dose_g * 2u)
                            : old_blob.yield_g;
    std::memcpy(dst.name, old_blob.name, kNameLen);
    dst.grind_anchor  = old_blob.grind_anchor;
    dst.color         = kDefaultPresetColor;

    err = nvs_set_blob(h, key, &dst, sizeof(dst));
    if (err != ESP_OK) return err;
    if (out_migrated) *out_migrated = true;
    return ESP_OK;
  }

  if (sz == sizeof(PresetV1)) {
    PresetV1 old_blob = {};
    err = nvs_get_blob(h, key, &old_blob, &sz);
    if (err != ESP_OK) return err;

    Preset& dst = s_table[i];
    dst = {};
    dst.version       = kPresetVersion;
    std::memcpy(dst.name, old_blob.name, kNameLen);
    dst.target_time_s = old_blob.target_time_s;
    dst.dose_g        = old_blob.dose_g;
    // Same backfill rule the pre-color path uses — yield from dose at the
    // default espresso ratio.
    dst.yield_g       = static_cast<uint8_t>(old_blob.dose_g * 2u);
    // v1 click_anchor was always 0 in the seeded defaults (no UI to edit
    // it), so the user's confirmed grind setting is the only useful seed
    // for the new float field.
    dst.grind_anchor  = kDefaultGrindAnchor;
    dst.color         = kDefaultPresetColor;

    err = nvs_set_blob(h, key, &dst, sizeof(dst));
    if (err != ESP_OK) return err;
    if (out_migrated) *out_migrated = true;
    return ESP_OK;
  }

  ESP_LOGW(kTag, "preset %u has unexpected blob size %u (expected %u, %u, or %u) — reseeding",
           i, static_cast<unsigned>(sz),
           static_cast<unsigned>(sizeof(Preset)),
           static_cast<unsigned>(sizeof(PresetV3)),
           static_cast<unsigned>(sizeof(PresetV1)));
  return ESP_ERR_NVS_NOT_FOUND;
}

esp_err_t load_table(nvs_handle_t h) {
  // kKeyCount is the "initialized" sentinel — its absence means a fresh
  // device that needs seeding. The value itself is no longer authoritative
  // (slot count is fixed at kMaxPresets); each slot's existence is
  // determined by whether its own blob is present.
  uint8_t marker = 0;
  esp_err_t err = nvs_get_u8(h, kKeyCount, &marker);
  if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_ERR_NVS_NOT_FOUND;
  if (err != ESP_OK) return err;

  bool any_migrated = false;
  for (uint8_t i = 0; i < kMaxPresets; ++i) {
    err = load_one_preset(h, i, &any_migrated);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
      // No blob for this slot — it's inactive. Leaving s_table[i] zeroed
      // (version == 0) is what is_active() reads.
      s_table[i] = {};
      continue;
    }
    if (err != ESP_OK) {
      // Unexpected NVS error on this one slot — don't kill the whole
      // table; just mark this slot inactive and keep going.
      ESP_LOGW(kTag, "preset %u load failed (%s) — marking inactive",
               i, esp_err_to_name(err));
      s_table[i] = {};
    }
  }
  if (any_migrated) {
    err = nvs_commit(h);
    if (err != ESP_OK) return err;
    ESP_LOGW(kTag, "migrated preset table to v%u (color seeded, yield_g backfilled where needed)",
             static_cast<unsigned>(kPresetVersion));
  }
  uint8_t sel = 0;
  err = nvs_get_u8(h, kKeySel, &sel);
  if (err == ESP_ERR_NVS_NOT_FOUND) sel = 0;
  else if (err != ESP_OK)           return err;
  if (sel >= kMaxPresets) sel = 0;
  // If the persisted selection landed on an inactive slot (possible after
  // a future clear() between reboots), fall forward to the next active one.
  if (s_table[sel].version == 0) {
    for (uint8_t i = 1; i <= kMaxPresets; ++i) {
      const uint8_t cand = static_cast<uint8_t>((sel + i) % kMaxPresets);
      if (s_table[cand].version != 0) { sel = cand; break; }
    }
  }
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
  uint8_t active = 0;
  for (uint8_t i = 0; i < kMaxPresets; ++i) if (s_table[i].version) ++active;
  ESP_LOGI(kTag, "%u of %u preset slots active, selected=%u",
           static_cast<unsigned>(active),
           static_cast<unsigned>(kMaxPresets),
           static_cast<unsigned>(s_selected));
  return ESP_OK;
}

uint8_t count() {
  if (!s_inited) return 0;
  uint8_t n = 0;
  for (uint8_t i = 0; i < kMaxPresets; ++i) if (s_table[i].version) ++n;
  return n;
}

bool is_active(uint8_t id) {
  if (!s_inited || id >= kMaxPresets) return false;
  return s_table[id].version != 0;
}

uint8_t selected_id() { return s_selected; }

Preset get(uint8_t id) {
  if (id >= kMaxPresets) return Preset{};
  return s_table[id];
}

uint8_t cycle_selected() {
  if (!s_inited) return 0;
  for (uint8_t i = 1; i <= kMaxPresets; ++i) {
    const uint8_t cand = static_cast<uint8_t>((s_selected + i) % kMaxPresets);
    if (s_table[cand].version != 0) {
      s_selected = cand;
      break;
    }
  }
  // No-op if no other active slot exists; s_selected stays put.

  nvs_handle_t h;
  if (nvs_open(kNamespace, NVS_READWRITE, &h) == ESP_OK) {
    nvs_set_u8(h, kKeySel, s_selected);
    nvs_commit(h);
    nvs_close(h);
  }
  return s_selected;
}

esp_err_t set(uint8_t id, const Preset& p) {
  if (!s_inited || id >= kMaxPresets) return ESP_ERR_INVALID_ARG;

  Preset stamped = p;
  stamped.version = kPresetVersion;

  nvs_handle_t h;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  char key[4];
  preset_key(id, key);
  err = nvs_set_blob(h, key, &stamped, sizeof(stamped));
  if (err == ESP_OK) err = nvs_commit(h);
  nvs_close(h);
  if (err == ESP_OK) s_table[id] = stamped;
  return err;
}

esp_err_t clear(uint8_t id) {
  if (!s_inited || id >= kMaxPresets) return ESP_ERR_INVALID_ARG;

  nvs_handle_t h;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
  if (err != ESP_OK) return err;
  char key[4];
  preset_key(id, key);
  err = nvs_erase_key(h, key);
  // Erasing a key that doesn't exist is fine — the slot is already inactive.
  if (err == ESP_ERR_NVS_NOT_FOUND) err = ESP_OK;
  if (err == ESP_OK) {
    s_table[id] = {};
    // If we just cleared the selected slot, advance to the next active
    // one so the UI never points at an empty slot.
    if (s_selected == id) {
      for (uint8_t i = 1; i <= kMaxPresets; ++i) {
        const uint8_t cand = static_cast<uint8_t>((id + i) % kMaxPresets);
        if (s_table[cand].version != 0) {
          s_selected = cand;
          break;
        }
      }
      nvs_set_u8(h, kKeySel, s_selected);
    }
    nvs_commit(h);
  }
  nvs_close(h);
  return err;
}

float last_grind(uint8_t id) {
  if (id >= kMaxPresets) return 0.0f;
  const float fallback = s_table[id].grind_anchor;
  nvs_handle_t h;
  if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) return fallback;
  char key[4];
  grind_key(id, key);
  uint32_t bits = 0;
  const esp_err_t err = nvs_get_u32(h, key, &bits);
  nvs_close(h);
  if (err != ESP_OK) return fallback;
  float v;
  std::memcpy(&v, &bits, sizeof(v));
  return v;
}

void set_last_grind(uint8_t id, float v) {
  if (id >= kMaxPresets) return;
  nvs_handle_t h;
  if (nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) return;
  char key[4];
  grind_key(id, key);
  uint32_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  nvs_set_u32(h, key, bits);
  nvs_commit(h);
  nvs_close(h);
}

}  // namespace espressopost::presets
