#pragma once

#include <cstdint>

#include "esp_err.h"

namespace espressopost::storage {

// Bit positions for ShotRecord::flags.
//   kFlagTombstone — user deleted this shot; the model must exclude it.
//   kFlagAnomaly   — user marked this shot as an outlier (test pull, channel,
//                    bad puck prep, etc.); excluded from training, kept for
//                    audit.
// Bits 2-7 are reserved. Sync state intentionally does NOT live here —
// append-only LittleFS records stay immutable; cloud sync (when it lands) is
// tracked by a high-water mark in NVS, not per-record.
constexpr uint8_t kFlagTombstone = 1u << 0;
constexpr uint8_t kFlagAnomaly   = 1u << 1;

// Bit positions for ShotRecord::taste_flags (v4+). Independent toggles the
// user picks on the Report screen as journal data — currently NOT consumed by
// the model. Each describes a taste impression or a visible pull defect; they
// are not mutually exclusive (a shot can read as both bitter and astringent).
// Bits 6-7 are reserved for future descriptors. On v3 records the byte reads
// as zero (all toggles off), which is the correct "unknown" semantic.
constexpr uint8_t kTasteBitter     = 1u << 0;
constexpr uint8_t kTasteSour       = 1u << 1;
constexpr uint8_t kTasteAstringent = 1u << 2;
constexpr uint8_t kTasteWatery     = 1u << 3;
constexpr uint8_t kTasteChanneled  = 1u << 4;
constexpr uint8_t kTasteBalanced   = 1u << 5;

// Fixed-width on-disk record for one espresso shot.
//
// Layout is packed and explicit so the file is portable and parseable without
// the device — the model (Step 5) and any future host-side tooling read the
// same bytes. Endianness is little-endian (ESP32 native); no host platform we
// care about is big-endian.
//
// `version` lets future schema changes be detected without a separate file
// header. v2 (32 B) used `click_delta` (int8) + 2 padding bytes; v3 (40 B)
// replaces both with two float fields — the user's *actual* grind setting
// (`user_grind`, the model's primary regressor) and the model's standing
// recommendation (`suggested_grind`). A one-shot migration in storage::init
// rewrites any v2 records present at first v3 boot. v4 keeps the v3 40-byte
// layout unchanged and only carves `taste_flags` out of the reserved space;
// v3 records are read as-is and their reserved bytes naturally read as zero
// (all taste toggles off) — no on-disk migration needed. v5 reinterprets the
// byte at offset 2: v4's `time_delta_s` (int8, shot time vs preset target) is
// replaced by `actual_time_s` (uint8, raw shot seconds). Storing the absolute
// time decouples the record from the preset's target_time_s — so changing a
// preset's target doesn't silently invalidate the history. The v4→v5 migration
// runs after presets::init and rewrites each record as
// `actual = clamp(delta + preset[id].target_time_s, 0..255)`.
//
// `rtc_epoch_s` is 0 if the PCF85063 hasn't been seeded yet — fall back to
// `timestamp_us` (esp_timer ticks since boot) for ordering then.
struct __attribute__((packed)) ShotRecord {
  uint8_t  version;          // 5 (current)
  uint8_t  preset_id;        // index into presets::get(); persisted in NVS
  uint8_t  actual_time_s;    // user-reported raw shot time in seconds (0..255). Replaced v4's int8 `time_delta_s` so the record is self-contained against future preset target_time_s changes.
  uint8_t  quality_stars;    // user-reported taste, 1..5
  uint8_t  flags;            // see kFlag* above; 0 = normal, immutable in v1
  uint8_t  confidence_pct;   // model's confidence at submit time, 0..95 in 5-unit steps; 0 = unknown/suppressed. Carved out of the v3 reserved space — old records naturally read 0 ("unknown"), no migration needed.
  uint8_t  taste_flags;      // see kTaste* above; bitfield of user-reported taste/defect toggles. v4 addition — v3 records read as 0 ("none reported").
  uint8_t  _reserved[1];     // future use; zero on write
  int64_t  timestamp_us;     // esp_timer microseconds since boot at submit
  uint32_t rtc_epoch_s;      // wall-clock seconds since 1970 — 0 until RTC set
  float    temp_c;
  float    humidity_pct;
  float    pressure_hpa;
  float    user_grind;       // absolute grind setting the user dialed (units = whatever the grinder's dial reads)
  float    suggested_grind;  // model recommendation at shot time; NaN until Step 5 emits values
};
static_assert(sizeof(ShotRecord) == 40, "ShotRecord wire size must be stable");

// Mount LittleFS on the `storage` partition (label matches partitions.csv),
// then run any preset-independent migrations (currently v2 → v3). Formats the
// partition on first boot or after corruption. Safe to call once. Returns
// ESP_ERR_INVALID_STATE on a second call.
esp_err_t init();

// Run any migrations that require the presets table to be loaded (currently
// v4 → v5, which rewrites stored time deltas as absolute brew seconds using
// each shot's preset target_time_s). Must be called after `presets::init()`
// and before `model::init()` reads the shot log. Also emits the temporary
// shot-log dump so the dump format matches the post-migration schema.
esp_err_t finalize_migrations();

// Append one shot record to the log. The record's `version` and `_pad` are
// overwritten internally — callers should leave them zeroed. Other fields are
// passed through verbatim. Flushes to flash before returning, so a power loss
// immediately after this returns will still retain the shot.
esp_err_t append_shot(const ShotRecord& record);

// Total number of records currently in the log. Cheap; reads file size and
// divides by sizeof(ShotRecord). Used by the UI to display "Saved #N".
uint32_t shot_count();

// Number of live (non-tombstoned) records tagged with `preset_id`. Scans the
// log, so O(n) rather than the stat() trick shot_count() uses. The preset editor
// reads this before a delete to warn how many saved posts the action will take
// with it.
uint32_t shot_count_for_preset(uint8_t preset_id);

// Physically remove every record tagged with `preset_id` from the log (not a
// tombstone — the bytes are gone). Rewrites the file via the same
// temp-file-then-rename path the migrations use, so a power loss mid-purge
// leaves the original log intact. A no-op success when nothing matches. Used
// when a preset is deleted so its history doesn't bleed into a future preset
// that reuses the slot index. The caller is responsible for a model::refit()
// afterward.
esp_err_t purge_preset_shots(uint8_t preset_id);

// Bulk-load every record in the log into `out`. Returns the number of records
// actually read; never exceeds `max`. Records arrive in the order they were
// appended (chronological by submit time). Used by the model (Step 5) to fit
// from history — at expected shot counts (tens to hundreds) reading the whole
// file is cheap and keeps the model agnostic of incremental update logic.
size_t read_shots(ShotRecord* out, size_t max);

}  // namespace espressopost::storage
