#include "cloud_json.hpp"

#include <cmath>
#include <cstdio>
#include <vector>

namespace espressopost::cloud::json {
namespace {

// Format a float for JSON: %g (shortest round-trippable-ish decimal at 6 sig
// figs, e.g. 5.2 not 5.2000), with the decimal point forced to '.'. We never
// call setlocale, so the C locale already gives '.', but the swap is cheap
// insurance against a locale slipping in. NaN/Inf must never reach here —
// callers map them to `null` or omit the field.
std::string fmt_float(float v) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%g", static_cast<double>(v));
  std::string s(buf);
  for (char& c : s) {
    if (c == ',') c = '.';
  }
  return s;
}

// Append `"key":` to out.
void key(std::string& out, const char* k) {
  out += '"';
  out += k;
  out += "\":";
}

// Append a numeric float field, or `null` if NaN/Inf.
void num_field(std::vector<std::string>& fields, const char* k, float v) {
  std::string f;
  key(f, k);
  f += std::isfinite(v) ? fmt_float(v) : "null";
  fields.push_back(std::move(f));
}

// The taste-bit names, indexed so name[i] pairs with bit (1u << i). Keeping the
// order tied to the bit positions makes the emitted array stable and matches
// the storage::kTaste* layout the bits mirror.
constexpr const char* kTasteNames[] = {
    "bitter", "sour", "astringent", "watery", "channeled", "balanced",
};

std::string taste_array(uint8_t taste_flags) {
  std::string out = "[";
  bool first = true;
  for (uint8_t i = 0; i < sizeof(kTasteNames) / sizeof(kTasteNames[0]); ++i) {
    if (!(taste_flags & (1u << i))) continue;
    if (!first) out += ',';
    out += '"';
    out += kTasteNames[i];
    out += '"';
    first = false;
  }
  out += ']';
  return out;
}

}  // namespace

std::string escape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (unsigned char c : s) {
    switch (c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b";  break;
      case '\f': out += "\\f";  break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if (c < 0x20) {
          char u[8];
          std::snprintf(u, sizeof(u), "\\u%04x", c);
          out += u;
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

void append_record(std::string& out, const ShotJson& r) {
  std::vector<std::string> fields;
  fields.reserve(16);

  auto int_field = [&](const char* k, long long v) {
    std::string f;
    key(f, k);
    f += std::to_string(v);
    fields.push_back(std::move(f));
  };
  auto bool_field = [&](const char* k, bool v) {
    std::string f;
    key(f, k);
    f += v ? "true" : "false";
    fields.push_back(std::move(f));
  };

  int_field("index", r.index);
  int_field("v", r.version);
  int_field("preset", r.preset_id);
  int_field("time_s", r.actual_time_s);
  int_field("stars", r.quality_stars);
  int_field("conf", r.confidence_pct);
  num_field(fields, "grind", r.user_grind);
  if (!std::isnan(r.suggested_grind)) num_field(fields, "suggested", r.suggested_grind);
  num_field(fields, "temp_c", r.temp_c);
  num_field(fields, "rh", r.humidity_pct);
  num_field(fields, "hpa", r.pressure_hpa);

  {
    std::string f;
    key(f, "taste");
    f += taste_array(r.taste_flags);
    fields.push_back(std::move(f));
  }
  bool_field("anomaly", (r.flags & bits::kFlagAnomaly) != 0);
  bool_field("tombstone", (r.flags & bits::kFlagTombstone) != 0);
  if (r.rtc_epoch_s != 0) int_field("epoch", r.rtc_epoch_s);
  int_field("boot_us", r.timestamp_us);

  out += '{';
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i) out += ',';
    out += fields[i];
  }
  out += '}';
}

std::string build_payload(const std::string& token, const ShotJson* recs, size_t n) {
  std::string out = "{\"token\":\"";
  out += escape(token);
  out += "\",\"shots\":[";
  for (size_t i = 0; i < n; ++i) {
    if (i) out += ',';
    append_record(out, recs[i]);
  }
  out += "]}";
  return out;
}

}  // namespace espressopost::cloud::json
