#include "touch.hpp"

#include "board_pins.hpp"
#include "display.hpp"

#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"

namespace espressopost::touch {
namespace {

constexpr const char* kTag = "touch";

i2c_master_bus_handle_t s_i2c_bus = nullptr;
esp_lcd_panel_io_handle_t s_tp_io = nullptr;
esp_lcd_touch_handle_t s_tp_handle = nullptr;
lv_indev_t* s_indev = nullptr;

void read_cb(lv_indev_t* /*indev*/, lv_indev_data_t* data) {
  esp_lcd_touch_point_data_t point = {};
  uint8_t point_count = 0;

  // esp_lcd_touch caches readings between read_data + get_data so the
  // controller is only hit once per LVGL frame.
  esp_lcd_touch_read_data(s_tp_handle);
  const esp_err_t err = esp_lcd_touch_get_data(
      s_tp_handle, &point, &point_count, /*max_point_cnt=*/1);

  if (err == ESP_OK && point_count > 0) {
    data->point.x = point.x;
    data->point.y = point.y;
    data->state = LV_INDEV_STATE_PRESSED;
  } else {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

esp_err_t init_i2c() {
  const i2c_master_bus_config_t bus_cfg = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = static_cast<gpio_num_t>(board::kI2cSda),
      .scl_io_num = static_cast<gpio_num_t>(board::kI2cScl),
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags = {
          .enable_internal_pullup = true,
      },
  };
  return i2c_new_master_bus(&bus_cfg, &s_i2c_bus);
}

esp_err_t init_panel(lv_display_t* disp) {
  esp_lcd_panel_io_i2c_config_t tp_io_cfg = ESP_LCD_TOUCH_IO_I2C_CST9217_CONFIG();
  // The CST9217 macro doesn't set scl_speed_hz; the new i2c_master driver
  // rejects a zero value with ESP_ERR_INVALID_ARG (`invalid scl frequency`).
  tp_io_cfg.scl_speed_hz = board::kI2cFreqHz;

  // C++ overload resolves to esp_lcd_new_panel_io_i2c_v2 because s_i2c_bus
  // is an i2c_master_bus_handle_t; no cast required.
  ESP_RETURN_ON_ERROR(
      esp_lcd_new_panel_io_i2c(s_i2c_bus, &tp_io_cfg, &s_tp_io),
      kTag, "panel_io_i2c");

  const esp_lcd_touch_config_t tp_cfg = {
      .x_max = board::kLcdHRes,
      .y_max = board::kLcdVRes,
      .rst_gpio_num = static_cast<gpio_num_t>(board::kTouchReset),
      .int_gpio_num = static_cast<gpio_num_t>(board::kTouchInt),
      .levels = {
          .reset = 0,
          .interrupt = 0,
      },
      .flags = {
          .swap_xy = 0,
          // CST9217's native frame is rotated 180° from the CO5300's visible
          // orientation on this board — without both mirrors a touch at the
          // top-right reports as bottom-left.
          .mirror_x = 1,
          .mirror_y = 1,
      },
  };
  ESP_RETURN_ON_ERROR(esp_lcd_touch_new_i2c_cst9217(s_tp_io, &tp_cfg, &s_tp_handle),
                      kTag, "new_touch_cst9217");

  // The LVGL task is already running (started by display::init()); hold the
  // lock while mutating LVGL's indev list so we don't race with the task's
  // input-read pass inside lv_timer_handler().
  if (!display::lock()) return ESP_ERR_TIMEOUT;
  s_indev = lv_indev_create();
  if (s_indev == nullptr) {
    display::unlock();
    return ESP_ERR_NO_MEM;
  }
  lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_display(s_indev, disp);
  lv_indev_set_read_cb(s_indev, read_cb);
  display::unlock();

  return ESP_OK;
}

}  // namespace

esp_err_t init(lv_display_t* disp) {
  if (disp == nullptr) return ESP_ERR_INVALID_ARG;
  if (s_tp_handle != nullptr) return ESP_ERR_INVALID_STATE;
  ESP_RETURN_ON_ERROR(init_i2c(), kTag, "i2c");
  ESP_RETURN_ON_ERROR(init_panel(disp), kTag, "panel");
  ESP_LOGI(kTag, "CST9217 touch ready");
  return ESP_OK;
}

}  // namespace espressopost::touch
