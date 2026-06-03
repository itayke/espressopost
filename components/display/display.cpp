#include "display.hpp"

#include "board_pins.hpp"

#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace espressopost::display {

// Has external linkage (within the espressopost::display namespace) so lock()
// and unlock() at the bottom of the file can touch it.
SemaphoreHandle_t s_lvgl_mutex = nullptr;

namespace {

constexpr const char* kTag = "display";

constexpr spi_host_device_t kSpiHost = SPI2_HOST;
constexpr int               kLcdBitsPerPixel = 16;  // RGB565 over QSPI

// Partial draw buffer: 50 lines × 466 px × 2 bytes = ~46 KiB per buffer.
// Two buffers in internal SRAM keep flush in lockstep with rendering without
// trashing the cache. Full-frame would be ~434 KiB — fits in PSRAM but slower.
constexpr int kDrawBufLines = 50;
constexpr size_t kDrawBufBytes =
    static_cast<size_t>(kDrawBufLines) * board::kLcdHRes * sizeof(uint16_t);

esp_lcd_panel_io_handle_t s_panel_io = nullptr;
esp_lcd_panel_handle_t    s_panel    = nullptr;
lv_display_t*             s_lvgl_disp = nullptr;

void* s_draw_buf_a = nullptr;
void* s_draw_buf_b = nullptr;
TaskHandle_t s_lvgl_task = nullptr;

bool on_color_trans_done(esp_lcd_panel_io_handle_t /*io*/,
                         esp_lcd_panel_io_event_data_t* /*evt*/,
                         void* user_ctx) {
  auto* disp = static_cast<lv_display_t*>(user_ctx);
  lv_display_flush_ready(disp);
  return false;
}

void flush_cb(lv_display_t* /*disp*/, const lv_area_t* area, uint8_t* px_map) {
  const int x1 = area->x1;
  const int y1 = area->y1;
  const int x2 = area->x2 + 1;
  const int y2 = area->y2 + 1;
  // LVGL is configured with LV_COLOR_FORMAT_RGB565_SWAPPED so the sw renderer
  // writes pixels in the big-endian order the CO5300 expects directly — no
  // CPU byte-swap on the flush hot path. (Earlier the swap ate ~10 ms per
  // partial flush; killed the ring drag's frame rate.)
  esp_lcd_panel_draw_bitmap(s_panel, x1, y1, x2, y2, px_map);
}

// The CO5300 over QSPI requires the column/row window to be 2-pixel aligned
// (each QSPI write carries a pair of RGB565 pixels). LVGL by default may
// invalidate areas with odd x1 / odd width, which makes the panel walk one
// pixel off per row and shears the image diagonally. Snap every invalidated
// area to even-x1/odd-x2 boundaries before LVGL renders into the partial
// buffer — matches Waveshare's BSP rounder for this exact board.
void rounder_event_cb(lv_event_t* e) {
  auto* area = static_cast<lv_area_t*>(lv_event_get_param(e));
  area->x1 = (area->x1 >> 1) << 1;
  area->y1 = (area->y1 >> 1) << 1;
  area->x2 = ((area->x2 >> 1) << 1) | 1;
  area->y2 = ((area->y2 >> 1) << 1) | 1;
}

void lvgl_tick_cb(void* /*arg*/) {
  // esp_timer ticks at microsecond resolution; we registered a 2 ms period.
  lv_tick_inc(2);
}

void lvgl_task(void* /*arg*/) {
  ESP_LOGI(kTag, "LVGL task started");
  for (;;) {
    uint32_t time_to_next_ms = 30;
    if (xSemaphoreTakeRecursive(s_lvgl_mutex, portMAX_DELAY) == pdTRUE) {
      time_to_next_ms = lv_timer_handler();
      xSemaphoreGiveRecursive(s_lvgl_mutex);
    }
    if (time_to_next_ms > 30) time_to_next_ms = 30;
    if (time_to_next_ms < 2)  time_to_next_ms = 2;
    vTaskDelay(pdMS_TO_TICKS(time_to_next_ms));
  }
}

esp_err_t init_spi_bus() {
  const spi_bus_config_t bus_cfg = {
      .data0_io_num = board::kLcdQspiD0,
      .data1_io_num = board::kLcdQspiD1,
      .sclk_io_num  = board::kLcdQspiSclk,
      .data2_io_num = board::kLcdQspiD2,
      .data3_io_num = board::kLcdQspiD3,
      .max_transfer_sz = static_cast<int>(kDrawBufBytes) + 16,
      .flags = SPICOMMON_BUSFLAG_QUAD | SPICOMMON_BUSFLAG_MASTER,
  };
  return spi_bus_initialize(kSpiHost, &bus_cfg, SPI_DMA_CH_AUTO);
}

esp_err_t init_panel() {
  const esp_lcd_panel_io_spi_config_t io_cfg = CO5300_PANEL_IO_QSPI_CONFIG(
      board::kLcdCs,
      /*on_color_trans_done=*/nullptr,
      /*user_ctx=*/nullptr);
  ESP_RETURN_ON_ERROR(
      esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(kSpiHost),
                               &io_cfg, &s_panel_io),
      kTag, "panel_io_spi");

  co5300_vendor_config_t vendor_cfg = {
      .init_cmds      = nullptr,  // use driver defaults
      .init_cmds_size = 0,
      .flags = {
          .use_qspi_interface = 1,
      },
  };
  const esp_lcd_panel_dev_config_t panel_cfg = {
      .reset_gpio_num = board::kLcdReset,
      .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
      .bits_per_pixel = kLcdBitsPerPixel,
      .vendor_config  = &vendor_cfg,
  };
  ESP_RETURN_ON_ERROR(esp_lcd_new_panel_co5300(s_panel_io, &panel_cfg, &s_panel),
                      kTag, "new_panel_co5300");

  ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(s_panel), kTag, "panel_reset");
  ESP_RETURN_ON_ERROR(esp_lcd_panel_init(s_panel),  kTag, "panel_init");

  // Waveshare's BSP for this exact board applies a 6-pixel X offset because
  // the CO5300's framebuffer addresses 480 wide but only 466 columns are
  // wired to the visible AMOLED matrix, starting at column 6. Without this
  // every partial redraw lands 6 px off, producing tearing/garble at edges.
  ESP_RETURN_ON_ERROR(esp_lcd_panel_set_gap(s_panel, 6, 0), kTag, "set_gap");

  // CO5300 is OLED; brightness is set via the panel command in the driver.
  // Default brightness is high — fine for bringup; we'll add a brightness
  // controller alongside the burn-in policy later.
  ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(s_panel, true), kTag, "disp_on");

  return ESP_OK;
}

esp_err_t init_lvgl() {
  lv_init();

  s_lvgl_disp = lv_display_create(board::kLcdHRes, board::kLcdVRes);
  if (s_lvgl_disp == nullptr) return ESP_ERR_NO_MEM;

  // Draw buffers MUST be internal DMA RAM: the CO5300 QSPI panel's SPI master
  // DMA can't source color data from PSRAM (panel_io_spi_tx_color rejects it).
  // Internal headroom for these comes from keeping LVGL's own allocator pool in
  // PSRAM (see lv_port_mem.c / LV_USE_CUSTOM_MALLOC) — without that, the pool +
  // these buffers + the Wi-Fi stack don't all fit in internal RAM.
  s_draw_buf_a = heap_caps_malloc(kDrawBufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  s_draw_buf_b = heap_caps_malloc(kDrawBufBytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  if (s_draw_buf_a == nullptr || s_draw_buf_b == nullptr) {
    ESP_LOGE(kTag, "failed to allocate LVGL draw buffers (need %u B each)",
             static_cast<unsigned>(kDrawBufBytes));
    return ESP_ERR_NO_MEM;
  }

  lv_display_set_buffers(s_lvgl_disp, s_draw_buf_a, s_draw_buf_b,
                         kDrawBufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_color_format(s_lvgl_disp, LV_COLOR_FORMAT_RGB565_SWAPPED);
  lv_display_set_flush_cb(s_lvgl_disp, flush_cb);
  lv_display_add_event_cb(s_lvgl_disp, rounder_event_cb,
                          LV_EVENT_INVALIDATE_AREA, nullptr);

  // Register the panel's color-transfer-done callback so flush_ready fires
  // exactly when DMA finishes, not after a guesstimated delay.
  const esp_lcd_panel_io_callbacks_t cbs = {
      .on_color_trans_done = on_color_trans_done,
  };
  ESP_RETURN_ON_ERROR(
      esp_lcd_panel_io_register_event_callbacks(s_panel_io, &cbs, s_lvgl_disp),
      kTag, "register_color_trans_done");

  // LVGL tick from esp_timer — independent of the FreeRTOS scheduler so
  // animations stay smooth even when the LVGL task is briefly preempted.
  const esp_timer_create_args_t tick_args = {
      .callback = &lvgl_tick_cb,
      .name = "lvgl_tick",
  };
  esp_timer_handle_t tick_timer = nullptr;
  ESP_RETURN_ON_ERROR(esp_timer_create(&tick_args, &tick_timer), kTag, "tick_timer_create");
  ESP_RETURN_ON_ERROR(esp_timer_start_periodic(tick_timer, 2'000), kTag, "tick_timer_start");

  s_lvgl_mutex = xSemaphoreCreateRecursiveMutex();
  if (s_lvgl_mutex == nullptr) return ESP_ERR_NO_MEM;

  const BaseType_t task_ok = xTaskCreatePinnedToCore(
      lvgl_task, "lvgl", /*stack=*/8192, nullptr, /*prio=*/2, &s_lvgl_task, /*core=*/1);
  if (task_ok != pdPASS) return ESP_ERR_NO_MEM;

  return ESP_OK;
}

}  // namespace

esp_err_t init() {
  if (s_panel != nullptr) return ESP_ERR_INVALID_STATE;
  ESP_RETURN_ON_ERROR(init_spi_bus(), kTag, "spi_bus");
  ESP_RETURN_ON_ERROR(init_panel(),   kTag, "panel");
  ESP_RETURN_ON_ERROR(init_lvgl(),    kTag, "lvgl");
  ESP_LOGI(kTag, "display + LVGL ready (%dx%d, %d-line partial buffer)",
           board::kLcdHRes, board::kLcdVRes, kDrawBufLines);
  return ESP_OK;
}

lv_display_t* lvgl_display() { return s_lvgl_disp; }

bool lock(uint32_t timeout_ms) {
  if (s_lvgl_mutex == nullptr) return false;
  const TickType_t ticks = (timeout_ms == UINT32_MAX)
                               ? portMAX_DELAY
                               : pdMS_TO_TICKS(timeout_ms);
  return xSemaphoreTakeRecursive(s_lvgl_mutex, ticks) == pdTRUE;
}

void unlock() {
  if (s_lvgl_mutex != nullptr) xSemaphoreGiveRecursive(s_lvgl_mutex);
}

esp_err_t set_brightness(uint8_t pct) {
  if (s_panel == nullptr) return ESP_ERR_INVALID_STATE;
  if (pct > 100) pct = 100;
  // Don't call esp_lcd_panel_io_tx_param(io, 0x51, ...) directly — in QSPI
  // mode the CO5300 driver wraps every command byte with an opcode prefix
  // (see esp_lcd_co5300_spi.c::tx_param). The public set_brightness goes
  // through that wrapper; raw tx_param bypasses it and the panel ignores
  // the write.
  return esp_lcd_panel_co5300_set_brightness(s_panel, pct);
}

esp_err_t set_on(bool on) {
  if (s_panel == nullptr) return ESP_ERR_INVALID_STATE;
  return esp_lcd_panel_disp_on_off(s_panel, on);
}

}  // namespace espressopost::display
