#include "storage.hpp"

#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace espressopost::storage {
namespace {

constexpr const char* kTag           = "storage";
constexpr const char* kPartitionLbl  = "storage";   // matches partitions.csv
constexpr const char* kBasePath      = "/littlefs";
constexpr const char* kShotsPath     = "/littlefs/shots.bin";
constexpr uint8_t     kRecordVersion = 2;

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

  size_t total = 0, used = 0;
  if (esp_littlefs_info(kPartitionLbl, &total, &used) == ESP_OK) {
    ESP_LOGI(kTag, "littlefs mounted at %s (%u/%u KB used, %u shots logged)",
             kBasePath, static_cast<unsigned>(used / 1024),
             static_cast<unsigned>(total / 1024),
             static_cast<unsigned>(shot_count()));
  } else {
    ESP_LOGI(kTag, "littlefs mounted at %s (info unavailable)", kBasePath);
  }

  s_mounted = true;
  return ESP_OK;
}

esp_err_t append_shot(const ShotRecord& record) {
  if (!s_mounted) return ESP_ERR_INVALID_STATE;

  ShotRecord r = record;
  r.version       = kRecordVersion;
  r._reserved[0]  = 0;
  r._reserved[1]  = 0;

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

}  // namespace espressopost::storage
