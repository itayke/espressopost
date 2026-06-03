// Host-side unit tests for components/cloud/cloud_json.{hpp,cpp}. Build with
// tests/host/run.sh (cmake + ninja). The serializer has no IDF deps, so this is
// plain C++17 and gives a tight loop for getting the JSON wire format right
// (field omission, NaN handling, escaping) without flashing the device.

#define CATCH_CONFIG_MAIN
#include "catch.hpp"

#include "cloud_json.hpp"

#include <cmath>
#include <string>

using namespace espressopost::cloud::json;

namespace {

// A representative record: known suggestion absent (NaN), no RTC, no taste/flags.
// Mirrors what a typical early shot looks like on disk.
ShotJson make_record() {
  ShotJson r{};
  r.index          = 0;
  r.version        = 5;
  r.preset_id      = 0;
  r.actual_time_s  = 33;
  r.quality_stars  = 4;
  r.flags          = 0;
  r.confidence_pct = 80;
  r.taste_flags    = 0;
  r.timestamp_us   = 12345;
  r.rtc_epoch_s    = 0;
  r.temp_c         = 22.0f;
  r.humidity_pct   = 50.0f;
  r.pressure_hpa   = 1013.0f;
  r.user_grind     = 5.2f;
  r.suggested_grind = std::nanf("");
  return r;
}

std::string one(const ShotJson& r) {
  std::string out;
  append_record(out, r);
  return out;
}

}  // namespace

TEST_CASE("minimal record serializes to the exact expected object") {
  const std::string expected =
      "{\"index\":0,\"v\":5,\"preset\":0,\"time_s\":33,\"stars\":4,\"conf\":80,"
      "\"grind\":5.2,\"temp_c\":22,\"rh\":50,\"hpa\":1013,\"taste\":[],"
      "\"anomaly\":false,\"tombstone\":false,\"boot_us\":12345}";
  REQUIRE(one(make_record()) == expected);
}

TEST_CASE("suggested_grind: omitted when NaN, present otherwise") {
  ShotJson r = make_record();
  REQUIRE(one(r).find("\"suggested\"") == std::string::npos);

  r.suggested_grind = 5.15f;
  const std::string s = one(r);
  REQUIRE(s.find("\"suggested\":5.15") != std::string::npos);
}

TEST_CASE("epoch: omitted when 0, present when set") {
  ShotJson r = make_record();
  REQUIRE(one(r).find("\"epoch\"") == std::string::npos);

  r.rtc_epoch_s = 1717000000u;
  REQUIRE(one(r).find("\"epoch\":1717000000") != std::string::npos);
}

TEST_CASE("taste flags expand to a stable-ordered string array") {
  ShotJson r = make_record();
  // Set out of order (astringent then bitter) to prove the output order is
  // driven by bit position, not set order.
  r.taste_flags = bits::kTasteAstringent | bits::kTasteBitter;
  REQUIRE(one(r).find("\"taste\":[\"bitter\",\"astringent\"]") != std::string::npos);

  r.taste_flags = 0;
  REQUIRE(one(r).find("\"taste\":[]") != std::string::npos);

  r.taste_flags = bits::kTasteBalanced;
  REQUIRE(one(r).find("\"taste\":[\"balanced\"]") != std::string::npos);
}

TEST_CASE("flags expose anomaly and tombstone as booleans") {
  ShotJson r = make_record();
  r.flags = bits::kFlagTombstone | bits::kFlagAnomaly;
  const std::string s = one(r);
  REQUIRE(s.find("\"anomaly\":true") != std::string::npos);
  REQUIRE(s.find("\"tombstone\":true") != std::string::npos);
}

TEST_CASE("NaN climate values serialize as null, never the text nan") {
  ShotJson r = make_record();
  r.temp_c = std::nanf("");
  const std::string s = one(r);
  REQUIRE(s.find("\"temp_c\":null") != std::string::npos);
  REQUIRE(s.find("nan") == std::string::npos);
}

TEST_CASE("escape handles quotes, backslashes, and control chars") {
  REQUIRE(escape("a\"b") == "a\\\"b");
  REQUIRE(escape("a\\b") == "a\\\\b");
  REQUIRE(escape("a\nb") == "a\\nb");
  REQUIRE(escape("a\tb") == "a\\tb");
  REQUIRE(escape(std::string("a\x01""b")) == "a\\u0001b");
}

TEST_CASE("build_payload escapes the token and brackets the shots array") {
  ShotJson r = make_record();

  SECTION("zero shots") {
    const std::string s = build_payload("secret", &r, 0);
    REQUIRE(s == "{\"token\":\"secret\",\"shots\":[]}");
  }
  SECTION("one shot") {
    const std::string s = build_payload("secret", &r, 1);
    REQUIRE(s.rfind("{\"token\":\"secret\",\"shots\":[{", 0) == 0);
    REQUIRE(s.back() == '}');
    // exactly one object → no separating comma between objects
    REQUIRE(s.find("},{") == std::string::npos);
  }
  SECTION("many shots are comma-separated") {
    ShotJson recs[3] = {make_record(), make_record(), make_record()};
    const std::string s = build_payload("secret", recs, 3);
    size_t pos = 0, count = 0;
    while ((pos = s.find("},{", pos)) != std::string::npos) { ++count; ++pos; }
    REQUIRE(count == 2);  // 3 objects → 2 separators
  }
  SECTION("token with a quote is escaped") {
    const std::string s = build_payload("a\"b", &r, 0);
    REQUIRE(s.rfind("{\"token\":\"a\\\"b\"", 0) == 0);
  }
}

TEST_CASE("float formatting is compact and locale-independent") {
  ShotJson r = make_record();
  r.user_grind = 5.2f;
  REQUIRE(one(r).find("\"grind\":5.2") != std::string::npos);
  // no comma decimal separator leaks in
  REQUIRE(one(r).find("5,2") == std::string::npos);
}
