#include "storage.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "presets.hpp"

namespace espressopost::storage {
namespace {

constexpr const char* kTag           = "storage";
constexpr const char* kPartitionLbl  = "storage";   // matches partitions.csv
constexpr const char* kBasePath      = "/littlefs";
constexpr const char* kShotsPath     = "/littlefs/shots.bin";
constexpr const char* kShotsTmpPath  = "/littlefs/shots.bin.tmp";
constexpr uint8_t     kRecordVersion = 5;

// One-time backfill value applied to v2 → v3 migration. Hardcoded because
// v2 had no grind field at all and the user has confirmed every existing
// post was ground at this setting; future migrations can pick their own
// strategy (NaN, per-record prompt, etc.) without inheriting this constant.
constexpr float kGrindBackfillV2 = 5.2f;

// On-disk v2 layout, kept here only so migrate_to_v3() can decode old files.
// Do NOT use this from new code paths — production reads/writes the current
// `ShotRecord` (v3) defined in the public header.
struct __attribute__((packed)) ShotRecordV2 {
  uint8_t  version;
  uint8_t  preset_id;
  int8_t   time_delta_s;
  uint8_t  quality_stars;
  int8_t   click_delta;
  uint8_t  flags;
  uint8_t  _reserved[2];
  int64_t  timestamp_us;
  uint32_t rtc_epoch_s;
  float    temp_c;
  float    humidity_pct;
  float    pressure_hpa;
};
static_assert(sizeof(ShotRecordV2) == 32, "v2 record size must match the schema we migrated away from");

// v3 + v4 share a 40-byte layout; v4 only carved `taste_flags` out of v3's
// reserved bytes (which read as zero — "none reported"). Both store the shot
// time as a signed delta against the preset's target_time_s at submit time.
// Kept here only so migrate_v4_to_v5() can decode the byte at offset 2 as a
// signed delta before rewriting the same record at v5 with an unsigned
// absolute brew time. Field-for-field identical to ShotRecord otherwise.
struct __attribute__((packed)) ShotRecordV4 {
  uint8_t  version;
  uint8_t  preset_id;
  int8_t   time_delta_s;
  uint8_t  quality_stars;
  uint8_t  flags;
  uint8_t  confidence_pct;
  uint8_t  taste_flags;
  uint8_t  _reserved[1];
  int64_t  timestamp_us;
  uint32_t rtc_epoch_s;
  float    temp_c;
  float    humidity_pct;
  float    pressure_hpa;
  float    user_grind;
  float    suggested_grind;
};
static_assert(sizeof(ShotRecordV4) == sizeof(ShotRecord),
              "v4 and v5 records share a fixed 40-byte size; only byte 2's semantics changed");

bool s_mounted = false;

// Append + count both touch the same file; a mutex avoids torn writes if the
// UI ever logs from two tasks (today it's just the LVGL task, but cheap to
// keep correct).
SemaphoreHandle_t s_lock = nullptr;

struct Guard {
  bool ok;
  explicit Guard(TickType_t timeout = portMAX_DELAY)
      : ok(xSemaphoreTake(s_lock, timeout) == pdTRUE) {}
  ~Guard() { if (ok) xSemaphoreGive(s_lock); }
};

// One-time rewrite of /littlefs/shots.bin from v1/v2 records (32 B) into the
// 40 B v3 layout. Runs once on the first boot after a schema bump and becomes
// a no-op forever after, gated by the first record's version byte. Output is
// stamped as v3 (not the current version) because this path has no presets
// access yet — `finalize_migrations()` later picks the records up and does
// the v3/v4 → v5 conversion that needs each preset's target_time_s.
//
// Safety: writes to shots.bin.tmp first and only renames on success, so a
// power loss mid-migration leaves the original file intact and the next
// boot retries. The temp file is also unlinked before each attempt so a
// stale leftover from a previous crashed run can't be appended to.
esp_err_t migrate_to_v3() {
  struct stat st = {};
  if (stat(kShotsPath, &st) != 0 || st.st_size == 0) {
    return ESP_OK;  // no shots yet — nothing to migrate
  }

  FILE* in = std::fopen(kShotsPath, "rb");
  if (!in) {
    ESP_LOGE(kTag, "migrate: open %s for read failed", kShotsPath);
    return ESP_FAIL;
  }

  uint8_t first_version = 0;
  if (std::fread(&first_version, 1, 1, in) != 1) {
    std::fclose(in);
    ESP_LOGE(kTag, "migrate: failed to read first byte");
    return ESP_FAIL;
  }
  // v3 and v4 share the 40-byte layout (see file-header comment), so
  // anything at v3 or newer is already current — no rewrite needed.
  if (first_version >= 3) {
    std::fclose(in);
    return ESP_OK;
  }
  // v1 and v2 share an identical 32-byte wire layout — v2 only renamed the
  // last three bytes from `_pad[3]` to `flags + _reserved[2]`. Reading a v1
  // record as ShotRecordV2 yields flags=0 + _reserved=0, which is the
  // correct semantic (v1 had no flags). The version byte is the only real
  // difference and we overwrite it during migration anyway.
  if (first_version < 1) {
    std::fclose(in);
    ESP_LOGE(kTag, "migrate: unsupported on-disk version %u",
             static_cast<unsigned>(first_version));
    return ESP_ERR_INVALID_VERSION;
  }
  if (st.st_size % sizeof(ShotRecordV2) != 0) {
    std::fclose(in);
    ESP_LOGE(kTag, "migrate: file size %ld not a multiple of pre-v3 record size %u",
             static_cast<long>(st.st_size), static_cast<unsigned>(sizeof(ShotRecordV2)));
    return ESP_ERR_INVALID_SIZE;
  }
  std::rewind(in);

  // Clear any leftover temp file from a previous interrupted attempt before
  // opening — fopen("wb") truncates, but the explicit unlink documents
  // intent and avoids subtle issues if littlefs ever changes truncation
  // semantics.
  unlink(kShotsTmpPath);

  FILE* out = std::fopen(kShotsTmpPath, "wb");
  if (!out) {
    std::fclose(in);
    ESP_LOGE(kTag, "migrate: open %s for write failed", kShotsTmpPath);
    return ESP_FAIL;
  }

  size_t count = 0;
  ShotRecordV2 old_rec = {};
  while (std::fread(&old_rec, 1, sizeof(old_rec), in) == sizeof(old_rec)) {
    // Output is v3, not the current schema, because this path runs before
    // presets are loaded — we don't have the target_time_s lookup needed for
    // the delta→actual conversion. `finalize_migrations()` will pick the v3
    // records up shortly after and convert them to v5 in a second pass.
    ShotRecordV4 new_rec = {};
    new_rec.version       = 3;
    new_rec.preset_id     = old_rec.preset_id;
    new_rec.time_delta_s  = old_rec.time_delta_s;
    new_rec.quality_stars = old_rec.quality_stars;
    new_rec.flags         = old_rec.flags;
    // _reserved already zeroed by `= {}`.
    new_rec.timestamp_us  = old_rec.timestamp_us;
    new_rec.rtc_epoch_s   = old_rec.rtc_epoch_s;
    new_rec.temp_c        = old_rec.temp_c;
    new_rec.humidity_pct  = old_rec.humidity_pct;
    new_rec.pressure_hpa  = old_rec.pressure_hpa;
    new_rec.user_grind    = kGrindBackfillV2;
    // No model output was emitted on v2 shots — surface that explicitly
    // rather than pretending the suggestion was 0.
    new_rec.suggested_grind = std::nanf("");

    if (std::fwrite(&new_rec, 1, sizeof(new_rec), out) != sizeof(new_rec)) {
      ESP_LOGE(kTag, "migrate: short write");
      std::fclose(in);
      std::fclose(out);
      unlink(kShotsTmpPath);
      return ESP_FAIL;
    }
    ++count;
  }
  std::fflush(out);
  const int outfd = fileno(out);
  if (outfd >= 0) fsync(outfd);
  std::fclose(out);
  std::fclose(in);

  // Atomic-ish swap. littlefs's rename replaces the target file in one
  // metadata operation, so on power loss we either see the old v2 file
  // (migration runs again next boot) or the new v3 file (done).
  if (std::rename(kShotsTmpPath, kShotsPath) != 0) {
    ESP_LOGE(kTag, "migrate: rename failed");
    unlink(kShotsTmpPath);
    return ESP_FAIL;
  }

  ESP_LOGW(kTag, "migrated %u shots from v2 → v3 (user_grind backfilled to %.2f)",
           static_cast<unsigned>(count), static_cast<double>(kGrindBackfillV2));
  return ESP_OK;
}

// Two-step rewrite of /littlefs/shots.bin: v3 and v4 records (40 B, with a
// signed delta-vs-target at byte 2) get converted to v5 (40 B, with an
// unsigned absolute brew time at byte 2). Each record's `actual_time_s` is
// rebuilt as `clamp(time_delta_s + presets::get(preset_id).target_time_s,
// 0..255)`. If the slot is now inactive its `target_time_s` reads as 0, so
// those records survive as raw deltas — best effort for orphaned data.
//
// Runs once after presets::init() has loaded the table. Becomes a no-op once
// all records on disk are at v5 or later. Same temp-file-then-rename safety
// as migrate_to_v3().
esp_err_t migrate_to_v5() {
  struct stat st = {};
  if (stat(kShotsPath, &st) != 0 || st.st_size == 0) return ESP_OK;

  FILE* in = std::fopen(kShotsPath, "rb");
  if (!in) {
    ESP_LOGE(kTag, "migrate v5: open %s for read failed", kShotsPath);
    return ESP_FAIL;
  }

  uint8_t first_version = 0;
  if (std::fread(&first_version, 1, 1, in) != 1) {
    std::fclose(in);
    return ESP_FAIL;
  }
  if (first_version >= 5) {
    std::fclose(in);
    return ESP_OK;
  }
  // Anything that survived migrate_to_v3 should be v3 or v4 (both 40-byte
  // layouts with delta at byte 2). v1/v2 would have been promoted to v3 by
  // the earlier pass; if first_version is below 3 here, the earlier pass
  // didn't run or failed — bail rather than misinterpret the bytes.
  if (first_version < 3) {
    std::fclose(in);
    ESP_LOGE(kTag, "migrate v5: unexpected on-disk version %u",
             static_cast<unsigned>(first_version));
    return ESP_ERR_INVALID_VERSION;
  }
  if (st.st_size % sizeof(ShotRecordV4) != 0) {
    std::fclose(in);
    ESP_LOGE(kTag, "migrate v5: file size %ld not a multiple of v3/v4 record size %u",
             static_cast<long>(st.st_size), static_cast<unsigned>(sizeof(ShotRecordV4)));
    return ESP_ERR_INVALID_SIZE;
  }
  std::rewind(in);

  unlink(kShotsTmpPath);
  FILE* out = std::fopen(kShotsTmpPath, "wb");
  if (!out) {
    std::fclose(in);
    ESP_LOGE(kTag, "migrate v5: open %s for write failed", kShotsTmpPath);
    return ESP_FAIL;
  }

  size_t count = 0;
  ShotRecordV4 old_rec = {};
  while (std::fread(&old_rec, 1, sizeof(old_rec), in) == sizeof(old_rec)) {
    const presets::Preset p = presets::get(old_rec.preset_id);
    const int target = static_cast<int>(p.target_time_s);  // 0 if slot inactive
    const int actual_i = std::clamp(old_rec.time_delta_s + target, 0, 255);

    ShotRecord new_rec = {};
    new_rec.version         = kRecordVersion;
    new_rec.preset_id       = old_rec.preset_id;
    new_rec.actual_time_s   = static_cast<uint8_t>(actual_i);
    new_rec.quality_stars   = old_rec.quality_stars;
    new_rec.flags           = old_rec.flags;
    new_rec.confidence_pct  = old_rec.confidence_pct;
    new_rec.taste_flags     = old_rec.taste_flags;
    // _reserved zeroed by `= {}`.
    new_rec.timestamp_us    = old_rec.timestamp_us;
    new_rec.rtc_epoch_s     = old_rec.rtc_epoch_s;
    new_rec.temp_c          = old_rec.temp_c;
    new_rec.humidity_pct    = old_rec.humidity_pct;
    new_rec.pressure_hpa    = old_rec.pressure_hpa;
    new_rec.user_grind      = old_rec.user_grind;
    new_rec.suggested_grind = old_rec.suggested_grind;

    if (std::fwrite(&new_rec, 1, sizeof(new_rec), out) != sizeof(new_rec)) {
      ESP_LOGE(kTag, "migrate v5: short write");
      std::fclose(in);
      std::fclose(out);
      unlink(kShotsTmpPath);
      return ESP_FAIL;
    }
    ++count;
  }
  std::fflush(out);
  const int outfd = fileno(out);
  if (outfd >= 0) fsync(outfd);
  std::fclose(out);
  std::fclose(in);

  if (std::rename(kShotsTmpPath, kShotsPath) != 0) {
    ESP_LOGE(kTag, "migrate v5: rename failed");
    unlink(kShotsTmpPath);
    return ESP_FAIL;
  }

  ESP_LOGW(kTag, "migrated %u shots from v3/v4 → v5 (delta + preset target → actual seconds)",
           static_cast<unsigned>(count));
  return ESP_OK;
}

}  // namespace

esp_err_t init() {
  if (s_mounted) return ESP_ERR_INVALID_STATE;

  s_lock = xSemaphoreCreateMutex();
  if (s_lock == nullptr) return ESP_ERR_NO_MEM;

  const esp_vfs_littlefs_conf_t conf = {
      .base_path              = kBasePath,
      .partition_label        = kPartitionLbl,
      .partition              = nullptr,
      .format_if_mount_failed = true,
      .read_only              = false,
      .dont_mount             = false,
      .grow_on_mount          = false,
  };
  const esp_err_t err = esp_vfs_littlefs_register(&conf);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "littlefs mount failed: %s", esp_err_to_name(err));
    return err;
  }

  // v2 → v3 runs here because it doesn't need the presets table. The second
  // hop (v3/v4 → v5, which DOES need preset target_time_s) defers to
  // finalize_migrations(), invoked from app_main after presets::init().
  // Failure on either path leaves the file alone and the boot continues —
  // worst case the model just can't read the log.
  const esp_err_t mig_err = migrate_to_v3();
  if (mig_err != ESP_OK) {
    ESP_LOGE(kTag, "shot record v2 → v3 migration failed: %s — leaving file untouched",
             esp_err_to_name(mig_err));
  }

  // Flip the mount flag before the summary line so shot_count()'s guard
  // doesn't short-circuit and log "0 shots logged" when there are actually
  // records on disk. The filesystem is genuinely usable from here on.
  s_mounted = true;

  size_t total = 0, used = 0;
  if (esp_littlefs_info(kPartitionLbl, &total, &used) == ESP_OK) {
    ESP_LOGI(kTag, "littlefs mounted at %s (%u/%u KB used, %u shots logged)",
             kBasePath, static_cast<unsigned>(used / 1024),
             static_cast<unsigned>(total / 1024),
             static_cast<unsigned>(shot_count()));
  } else {
    ESP_LOGI(kTag, "littlefs mounted at %s (info unavailable)", kBasePath);
  }

  return ESP_OK;
}

esp_err_t finalize_migrations() {
  if (!s_mounted) return ESP_ERR_INVALID_STATE;

  const esp_err_t mig_err = migrate_to_v5();
  if (mig_err != ESP_OK) {
    ESP_LOGE(kTag, "shot record v4 → v5 migration failed: %s — leaving file untouched",
             esp_err_to_name(mig_err));
  }

  // TEMPORARY DIAGNOSTIC — dump every shot on disk as a parseable line so the
  // host-test fixture can be built from real device history. Capped at 64 so
  // a future power user doesn't flood the boot log. Heap-allocated because a
  // stack array of 64 × 40 B = 2.5 KB overflows the main task's ~3.5 KB
  // stack (learned the hard way on the first flash). Remove once we've
  // copied the data into a test.
  constexpr size_t kMaxDump = 64;
  auto* dump_buf = static_cast<ShotRecord*>(std::malloc(sizeof(ShotRecord) * kMaxDump));
  if (dump_buf != nullptr) {
    const size_t n_dump = read_shots(dump_buf, kMaxDump);
    for (size_t i = 0; i < n_dump; ++i) {
      const auto& r = dump_buf[i];
      ESP_LOGI(kTag,
               "dump[%u]: ver=%u preset=%u t_s=%u stars=%u flags=0x%02x "
               "T=%.2f H=%.2f P=%.2f grind=%.2f sugg=%.2f conf=%u rtc=%u "
               "taste=0x%02x",
               static_cast<unsigned>(i), static_cast<unsigned>(r.version),
               static_cast<unsigned>(r.preset_id),
               static_cast<unsigned>(r.actual_time_s),
               static_cast<unsigned>(r.quality_stars),
               static_cast<unsigned>(r.flags),
               static_cast<double>(r.temp_c),
               static_cast<double>(r.humidity_pct),
               static_cast<double>(r.pressure_hpa),
               static_cast<double>(r.user_grind),
               static_cast<double>(r.suggested_grind),
               static_cast<unsigned>(r.confidence_pct),
               static_cast<unsigned>(r.rtc_epoch_s),
               static_cast<unsigned>(r.taste_flags));
    }
    std::free(dump_buf);
  }

  return ESP_OK;
}

esp_err_t append_shot(const ShotRecord& record) {
  if (!s_mounted) return ESP_ERR_INVALID_STATE;

  ShotRecord r = record;
  r.version       = kRecordVersion;
  r._reserved[0]  = 0;

  Guard g;
  if (!g.ok) return ESP_ERR_TIMEOUT;

  FILE* f = std::fopen(kShotsPath, "ab");
  if (!f) {
    ESP_LOGE(kTag, "fopen %s failed", kShotsPath);
    return ESP_FAIL;
  }
  const size_t written = std::fwrite(&r, 1, sizeof(r), f);
  // fflush + fsync — without fsync the FILE* buffer is in RAM only; a power
  // loss between fclose() and the next mount-time superblock flush can lose
  // the shot. fsync forces littlefs to commit before we return.
  std::fflush(f);
  const int fd = fileno(f);
  if (fd >= 0) fsync(fd);
  std::fclose(f);

  if (written != sizeof(r)) {
    ESP_LOGE(kTag, "short write (%u/%u bytes)",
             static_cast<unsigned>(written), static_cast<unsigned>(sizeof(r)));
    return ESP_FAIL;
  }
  return ESP_OK;
}

uint32_t shot_count() {
  if (!s_mounted) return 0;
  struct stat st = {};
  if (stat(kShotsPath, &st) != 0) return 0;  // file doesn't exist yet
  return static_cast<uint32_t>(st.st_size / sizeof(ShotRecord));
}

size_t read_shots(ShotRecord* out, size_t max) {
  if (!s_mounted || out == nullptr || max == 0) return 0;

  Guard g;
  if (!g.ok) return 0;

  FILE* f = std::fopen(kShotsPath, "rb");
  if (!f) return 0;  // no shots yet — not an error

  size_t n = 0;
  while (n < max && std::fread(&out[n], sizeof(ShotRecord), 1, f) == 1) ++n;
  std::fclose(f);
  return n;
}

}  // namespace espressopost::storage
