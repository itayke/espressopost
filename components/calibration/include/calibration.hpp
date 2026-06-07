#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

// Calibration boundaries — the marker primitive behind the quality-drift
// roadmap (Tier 0). A boundary is a wall-clock instant at which something about
// the setup changed; shots are bucketed into epochs by comparing their
// rtc_epoch_s to these boundaries *at read time*. Nothing is stamped onto the
// immutable ShotRecord, so a boundary can be added (or corrected) retroactively
// and re-bucket the entire existing log — which is the whole point: the user
// usually realizes a change happened *after* the fact (the ~2026-05-31 grinder
// recalibration was discovered days later).
//
// Storage is a single small NVS blob (namespace "calib"): the boundary array,
// length implied by the blob size. Append-only in spirit; remove_at exists only
// to fix a mistyped entry. No per-record schema change, no version bump.
namespace espressopost::calibration {

// Why a boundary was set. Three user-facing reasons collapsing to two model
// behaviors (see is_epoch):
//   Grinder — recalibrated the dial / swapped burrs. A HARD change: the model
//             fits a per-epoch offset so pre/post shots sit on one comparable
//             scale (climate slopes stay pooled across all epochs).
//   Machine — new or serviced espresso machine. Also HARD: it shifts brew time
//             at a fixed grind, so it earns its own per-epoch offset too (same
//             treatment as Grinder under the per-epoch-intercept model).
//   Beans   — new bag / roast. A SOFT reset of the *quality* baseline only, with
//             no effect on the grind axis. Stored now; consumed by the drift
//             detector (Tier 1). Ignored by epoch_index().
// Disk-stable: Grinder/Beans keep the original Epoch/Marker codes (0/1), so a
// boundary written by the earlier two-reason build still reads correctly.
enum class Kind : uint8_t { Grinder = 0, Beans = 1, Machine = 2 };

// Does this reason start a new grind/time epoch (vs. a soft quality marker)?
inline bool is_epoch(Kind k) { return k == Kind::Grinder || k == Kind::Machine; }

// One stored boundary. Packed so the NVS blob is compact and layout-stable.
struct __attribute__((packed)) Boundary {
  uint32_t rtc_epoch_s;  // wall-clock second the change took effect
  uint8_t  kind;         // Kind
};
static_assert(sizeof(Boundary) == 5, "Boundary blob layout must be stable");

// Plenty of headroom for a human-paced log of recalibrations and bag changes;
// also caps the model's extra per-epoch regressors at a sane number.
constexpr size_t kMaxBoundaries = 16;

// Load the boundary list from NVS. Call once, after nvs_flash is up
// (presets::init owns nvs_flash_init today) and before model::init reads the
// shot log. A fresh device starts empty. Returns ESP_ERR_INVALID_STATE on a
// second call; never fatal otherwise (a missing/corrupt blob → empty list).
esp_err_t init();

// Number of boundaries currently stored (epochs and markers together).
size_t count();

// Copy up to `max` boundaries into `out`, sorted ascending by rtc_epoch_s.
// Returns the number written.
size_t list(Boundary* out, size_t max);

// Append a boundary at wall-clock `rtc_epoch_s` and persist, keeping the list
// sorted. Rejects rtc_epoch_s == 0 (no wall clock to anchor it in time) with
// ESP_ERR_INVALID_ARG, and a full list with ESP_ERR_NO_MEM.
esp_err_t add(uint32_t rtc_epoch_s, Kind kind);

// Remove the boundary at sorted index `i` and persist. For correcting a
// mistyped entry from the UI. ESP_ERR_INVALID_ARG if out of range.
esp_err_t remove_at(size_t i);

// A shot's epoch index, derived from its wall-clock time. Epoch 0 is the oldest
// (before the first epoch boundary); each epoch (Grinder/Machine) boundary the
// shot is at or past advances the index by one. Beans markers are ignored here.
// A shot with rtc_epoch_s == 0 (RTC never seeded) has no timeline position →
// epoch 0.
uint8_t epoch_index(uint32_t shot_rtc_epoch_s);

// Number of distinct epochs = (# epoch boundaries) + 1. Always ≥ 1.
uint8_t epoch_count();

}  // namespace espressopost::calibration
