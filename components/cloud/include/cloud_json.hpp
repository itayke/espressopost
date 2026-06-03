#pragma once

// Pure, IDF-free serialization of shot records into the JSON body the device
// POSTs to the Google Apps Script Web App. Hand-rolled (no cJSON) on purpose:
// this layer has zero ESP-IDF includes so it builds and unit-tests on the host
// under tests/host/ — mirroring the model / model_math split. cloud.cpp copies
// fields from the on-disk storage::ShotRecord into a ShotJson and calls in here.

#include <cstdint>
#include <string>

namespace espressopost::cloud::json {

// Mirror of storage::kFlag* / kTaste* bit positions. Duplicated here rather than
// pulling in storage.hpp (which includes esp_err.h, an IDF header) so this layer
// stays host-buildable. cloud.cpp static_asserts these equal the storage
// originals, so the two definitions cannot silently drift apart.
namespace bits {
constexpr uint8_t kFlagTombstone = 1u << 0;
constexpr uint8_t kFlagAnomaly   = 1u << 1;

constexpr uint8_t kTasteBitter     = 1u << 0;
constexpr uint8_t kTasteSour       = 1u << 1;
constexpr uint8_t kTasteAstringent = 1u << 2;
constexpr uint8_t kTasteWatery     = 1u << 3;
constexpr uint8_t kTasteChanneled  = 1u << 4;
constexpr uint8_t kTasteBalanced   = 1u << 5;
}  // namespace bits

// Plain copy of the storage::ShotRecord fields this layer serializes, plus the
// record's absolute index in the log (used as a stable dedupe key on the sheet
// side). A separate POD keeps the serializer independent of the on-disk struct.
struct ShotJson {
  uint32_t index;            // absolute record position in the log (HWM + i)
  uint8_t  version;
  uint8_t  preset_id;
  uint8_t  actual_time_s;
  uint8_t  quality_stars;
  uint8_t  flags;            // bits::kFlag*
  uint8_t  confidence_pct;
  uint8_t  taste_flags;      // bits::kTaste*
  int64_t  timestamp_us;     // esp_timer ticks at submit; ordering fallback
  uint32_t rtc_epoch_s;      // wall-clock seconds, 0 if RTC unset
  float    temp_c;
  float    humidity_pct;
  float    pressure_hpa;
  float    user_grind;
  float    suggested_grind;  // NaN until the model emits a suggestion
};

// Escape a string's contents for embedding inside JSON double-quotes (does NOT
// add the surrounding quotes). Handles " \ and the C0 control set: \b \f \n \r
// \t are emitted as their short escapes, any other char < 0x20 as \u00XX.
std::string escape(const std::string& s);

// Append one record as a JSON object to `out`. Field order is fixed (so the
// output is deterministic and exact-string testable). Optional fields are
// omitted: `epoch` when rtc_epoch_s == 0, `suggested` when suggested_grind is
// NaN. NaN climate values serialize as JSON `null`, never the text "nan".
void append_record(std::string& out, const ShotJson& r);

// Build the full request body: {"token":"<esc>","shots":[ {…}, … ]}.
std::string build_payload(const std::string& token, const ShotJson* recs, size_t n);

}  // namespace espressopost::cloud::json
