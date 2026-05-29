#pragma once

#include <cstdint>

#include "esp_err.h"

namespace espressopost::presets {

// Fixed number of preset slots the user can address. Not all slots are
// necessarily active at any moment: a slot is "active" when an NVS blob
// exists for it, "inactive" when none does. The editor (when it lands)
// creates a slot by calling set() on an inactive id and deletes one by
// calling clear() — there is no add/remove; the slot itself is permanent
// and only the data inside it appears/disappears. Selection (tap-to-cycle)
// skips inactive slots without collapsing the numbering.
constexpr uint8_t kMaxPresets = 10;
constexpr uint8_t kNameLen    = 16;   // including NUL terminator

// One brew preset. Persisted as a packed blob under NVS key `pN` so the
// on-disk shape matches the in-memory shape — no per-field serialization.
//
// `version` lets future preset schema changes be detected without a separate
// catalog header — same trick we use on ShotRecord. v1 (20 B, no version
// byte, int8 click_anchor) is detected by blob size in presets::init() and
// transparently rewritten. v3 keeps the v2 24-byte layout unchanged and only
// repurposes the former `_pad` byte as `yield_g`; the v2→v3 migration uses
// the `version` byte (not size) as the disambiguator and backfills yield
// from dose at the classic espresso ratio.
//
// `grind_anchor` is the model's per-preset baseline grind setting, in the
// same absolute units the user dials on their grinder. Used as a starting
// hint for new shots (the Report UI pre-populates the grind stepper with
// this value) and as the prior peak for the Step-5 model.
//
// `yield_g` is the preset's target espresso output in grams. The brew ratio
// is implicit (`yield_g / dose_g`); no separate ratio field. Not surfaced
// on the ShotRecord — shots carry only the dialed grind + the time delta,
// not the per-shot yield.
//
// `name` is retained for wire-format stability but is no longer surfaced
// in the UI — slots are addressed by their 1-based index ("PRESET N").
// A future v4 schema bump may drop the field outright.
struct __attribute__((packed)) Preset {
  uint8_t version;          // 3 (current)
  uint8_t target_time_s;
  uint8_t dose_g;
  uint8_t yield_g;
  char    name[kNameLen];
  float   grind_anchor;
};
static_assert(sizeof(Preset) == 24, "Preset NVS blob size must be stable");

// Open the NVS namespace, seed the default preset table on first boot, and
// load the current selection. Safe to call once. Returns ESP_ERR_INVALID_STATE
// on a second call.
esp_err_t init();

// Number of currently active slots (0..kMaxPresets). May be 0 if the user
// has cleared every slot — callers should be prepared for an empty state.
uint8_t count();

// True if slot `id` holds a preset. Cheap; no NVS access. False for any
// out-of-range id and for any cleared slot.
bool is_active(uint8_t id);

// Which preset is currently active (0..kMaxPresets-1, always points at an
// active slot when any exist). Used by the Report screen and written into
// every ShotRecord.
uint8_t selected_id();

// Read-only view of a preset by id. Returns an empty Preset (all zeros,
// `version == 0`) if `id` is out of range or the slot is inactive.
Preset get(uint8_t id);

// Cycle to the next active preset, persist the selection to NVS, and return
// the new selected id. Skips inactive slots without collapsing the
// numbering (e.g. with slots 1/3/5 active, the sequence is 1→3→5→1). If
// only one slot is active the selection doesn't change.
uint8_t cycle_selected();

// Write a preset into slot `id`. Creates the slot if it was inactive and
// updates it in place otherwise. `version` is stamped automatically. The
// blob is committed to NVS before returning. Returns ESP_ERR_INVALID_ARG
// for an out-of-range id.
esp_err_t set(uint8_t id, const Preset& p);

// Delete the preset in slot `id`. If `id` was the selected slot, selection
// auto-advances to the next active slot (or stays put if no other active
// slot exists). Clearing an already-inactive slot is a no-op success.
// Returns ESP_ERR_INVALID_ARG for an out-of-range id.
esp_err_t clear(uint8_t id);

// Last grind value the user dialed for this preset, persisted across reboots.
// Falls back to the preset's `grind_anchor` if no value has been stored yet.
// Stored separately from the Preset blob so the schema stays stable across
// per-shot writes (which would otherwise rewrite the whole blob every click).
float last_grind(uint8_t id);

// Persist the last grind value for this preset. Cheap NVS u32 write + commit;
// called on every grind step in the Report UI. NVS wear at our usage volumes
// (≲ tens of clicks per day) is well inside the part's endurance budget.
void set_last_grind(uint8_t id, float v);

}  // namespace espressopost::presets
