#include "ui.hpp"

#include <algorithm>
#include <cstdio>

#include "climate.hpp"
#include "display.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "presets.hpp"
#include "storage.hpp"

namespace espressopost::ui {
namespace {

constexpr const char* kTag = "report";

// Display geometry — round panel, all widgets stay safely inside the 466 px
// circle.
constexpr int32_t kScreen   = 466;
constexpr int32_t kCenter   = kScreen / 2;

// Time-delta range. Wider than realistic so a long-pull-and-channel doesn't
// clip; the model will downweight outliers via the quality field anyway.
constexpr int8_t kDeltaMin     = -30;
constexpr int8_t kDeltaMax     =  30;
constexpr int8_t kDeltaStep    =   1;

constexpr uint8_t kMaxStars = 5;

// AMOLED-friendly muted palette — pure-black background, no max-intensity
// sub-pixels. Same logic as the bringup screen (see git history).
const lv_color_t kColorBg      = LV_COLOR_MAKE(0x00, 0x00, 0x00);
const lv_color_t kColorAccent  = LV_COLOR_MAKE(0xC8, 0x80, 0x36);
const lv_color_t kColorText    = LV_COLOR_MAKE(0xE0, 0xE0, 0xE0);
const lv_color_t kColorMuted   = LV_COLOR_MAKE(0x70, 0x70, 0x70);
const lv_color_t kColorDim     = LV_COLOR_MAKE(0x30, 0x30, 0x30);

lv_obj_t* s_preset_label  = nullptr;
lv_obj_t* s_climate_label = nullptr;
lv_obj_t* s_delta_label   = nullptr;
lv_obj_t* s_star_btns[kMaxStars] = {};
lv_obj_t* s_submit_btn    = nullptr;
lv_obj_t* s_submit_label  = nullptr;
lv_obj_t* s_toast_label   = nullptr;
lv_timer_t* s_toast_timer = nullptr;

// "Has the user touched this field at least once?" — Submit is gated on both.
bool   s_delta_set     = false;
int8_t s_delta_value   = 0;
uint8_t s_stars_value  = 0;   // 0 = none picked yet

void refresh_preset_label() {
  const auto id = presets::selected_id();
  const auto p  = presets::get(id);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%s  \xC2\xB7  target %us", p.name,
                static_cast<unsigned>(p.target_time_s));
  lv_label_set_text(s_preset_label, buf);
}

void on_preset_tap(lv_event_t*) {
  presets::cycle_selected();
  refresh_preset_label();
}

void refresh_delta_label() {
  if (!s_delta_set) {
    lv_label_set_text(s_delta_label, "--");
    lv_obj_set_style_text_color(s_delta_label, kColorMuted, LV_PART_MAIN);
    return;
  }
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%+ds", static_cast<int>(s_delta_value));
  lv_label_set_text(s_delta_label, buf);
  lv_obj_set_style_text_color(s_delta_label, kColorText, LV_PART_MAIN);
}

void refresh_stars() {
  for (uint8_t i = 0; i < kMaxStars; ++i) {
    const bool lit = (i < s_stars_value);
    lv_obj_set_style_bg_color(s_star_btns[i], lit ? kColorAccent : kColorDim,
                              LV_PART_MAIN);
  }
}

void refresh_submit_enabled() {
  const bool ready = s_delta_set && s_stars_value > 0;
  if (ready) {
    lv_obj_remove_state(s_submit_btn, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(s_submit_btn, kColorAccent, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_submit_label, kColorBg, LV_PART_MAIN);
  } else {
    lv_obj_add_state(s_submit_btn, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(s_submit_btn, kColorDim, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_submit_label, kColorMuted, LV_PART_MAIN);
  }
}

void reset_form() {
  s_delta_set    = false;
  s_delta_value  = 0;
  s_stars_value  = 0;
  refresh_delta_label();
  refresh_stars();
  refresh_submit_enabled();
}

void hide_toast(lv_timer_t* t) {
  lv_obj_add_flag(s_toast_label, LV_OBJ_FLAG_HIDDEN);
  lv_timer_delete(t);
  s_toast_timer = nullptr;
}

void show_toast(const char* text) {
  lv_label_set_text(s_toast_label, text);
  lv_obj_remove_flag(s_toast_label, LV_OBJ_FLAG_HIDDEN);
  if (s_toast_timer) lv_timer_delete(s_toast_timer);
  s_toast_timer = lv_timer_create(hide_toast, 1500, nullptr);
}

void on_delta_minus(lv_event_t*) {
  if (!s_delta_set) { s_delta_set = true; s_delta_value = 0; }
  s_delta_value = std::max<int8_t>(kDeltaMin, static_cast<int8_t>(s_delta_value - kDeltaStep));
  refresh_delta_label();
  refresh_submit_enabled();
}

void on_delta_plus(lv_event_t*) {
  if (!s_delta_set) { s_delta_set = true; s_delta_value = 0; }
  s_delta_value = std::min<int8_t>(kDeltaMax, static_cast<int8_t>(s_delta_value + kDeltaStep));
  refresh_delta_label();
  refresh_submit_enabled();
}

void on_star_tap(lv_event_t* e) {
  auto* btn = static_cast<lv_obj_t*>(lv_event_get_target(e));
  const auto idx = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
  // Tap a lit star to toggle it off; otherwise raise the rating to that star.
  if (s_stars_value == idx + 1) s_stars_value = static_cast<uint8_t>(idx);
  else                          s_stars_value = static_cast<uint8_t>(idx + 1);
  (void)btn;
  refresh_stars();
  refresh_submit_enabled();
}

void on_submit(lv_event_t*) {
  if (!(s_delta_set && s_stars_value > 0)) return;  // belt-and-suspenders

  const climate::Reading r = climate::latest();
  storage::ShotRecord rec = {};
  rec.preset_id     = presets::selected_id();
  rec.time_delta_s  = s_delta_value;
  rec.quality_stars = s_stars_value;
  rec.click_delta   = 0;                    // stub until the model lands
  rec.timestamp_us  = esp_timer_get_time();
  rec.rtc_epoch_s   = 0;                    // until RTC step
  if (r.timestamp_us != 0) {
    rec.temp_c       = r.temp_c;
    rec.humidity_pct = r.humidity_pct;
    rec.pressure_hpa = r.pressure_hpa;
  }

  const esp_err_t err = storage::append_shot(rec);
  if (err != ESP_OK) {
    ESP_LOGE(kTag, "append_shot failed: %s", esp_err_to_name(err));
    show_toast("save failed");
    return;
  }

  char buf[24];
  std::snprintf(buf, sizeof(buf), "Saved #%u", static_cast<unsigned>(storage::shot_count()));
  show_toast(buf);
  reset_form();
}

void update_climate_strip(lv_timer_t*) {
  const climate::Reading r = climate::latest();
  if (r.timestamp_us == 0) {
    lv_label_set_text(s_climate_label, "P --  H --  T --");
    return;
  }
  const float p_inhg = climate::hpa_to_inhg(r.pressure_hpa);
  const float t_f    = climate::c_to_f(r.temp_c);
  char buf[48];
  std::snprintf(buf, sizeof(buf), "P %.2finHg  H %.0f%%  T %.1f\xC2\xB0""F",
                p_inhg, r.humidity_pct, t_f);
  lv_label_set_text(s_climate_label, buf);
}

lv_obj_t* make_round_btn(lv_obj_t* parent, int32_t size) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, size, size);
  lv_obj_set_style_radius(b, size / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(b, kColorDim, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
  return b;
}

}  // namespace

void start_report() {
  if (!display::lock()) return;

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, kColorBg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // --- Preset row at the very top: tap to cycle. Made a button so LVGL
  // gives us a generous hit area without us having to position one manually.
  static lv_style_t row_text_style;
  lv_style_init(&row_text_style);
  lv_style_set_text_color(&row_text_style, kColorMuted);
  lv_style_set_text_font(&row_text_style, &lv_font_montserrat_14);

  lv_obj_t* preset_btn = lv_button_create(scr);
  lv_obj_set_style_bg_opa(preset_btn, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(preset_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(preset_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(preset_btn, 6, LV_PART_MAIN);
  lv_obj_align(preset_btn, LV_ALIGN_TOP_MID, 0, 40);
  lv_obj_add_event_cb(preset_btn, on_preset_tap, LV_EVENT_CLICKED, nullptr);
  s_preset_label = lv_label_create(preset_btn);
  lv_obj_add_style(s_preset_label, &row_text_style, LV_PART_MAIN);
  lv_obj_center(s_preset_label);
  refresh_preset_label();

  // --- Climate status strip just below the preset row ---
  s_climate_label = lv_label_create(scr);
  lv_obj_add_style(s_climate_label, &row_text_style, LV_PART_MAIN);
  lv_obj_align(s_climate_label, LV_ALIGN_TOP_MID, 0, 80);
  lv_label_set_text(s_climate_label, "P --  H --  T --");
  lv_timer_create(update_climate_strip, 1000, nullptr);

  // --- Time-delta stepper: [ − ]  ±Xs  [ + ] centered just above middle ---
  // "delta" caption above the value, in muted gray.
  lv_obj_t* delta_caption = lv_label_create(scr);
  lv_obj_set_style_text_color(delta_caption, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(delta_caption, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(delta_caption, "time vs target");
  lv_obj_align(delta_caption, LV_ALIGN_CENTER, 0, -100);

  s_delta_label = lv_label_create(scr);
  static lv_style_t delta_value_style;
  lv_style_init(&delta_value_style);
  lv_style_set_text_font(&delta_value_style, &lv_font_montserrat_48);
  lv_obj_add_style(s_delta_label, &delta_value_style, LV_PART_MAIN);
  lv_obj_align(s_delta_label, LV_ALIGN_CENTER, 0, -50);

  const int32_t step_size = 70;
  lv_obj_t* minus = make_round_btn(scr, step_size);
  lv_obj_align(minus, LV_ALIGN_CENTER, -120, -50);
  lv_obj_t* minus_lbl = lv_label_create(minus);
  lv_obj_set_style_text_color(minus_lbl, kColorText, LV_PART_MAIN);
  lv_obj_set_style_text_font(minus_lbl, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_label_set_text(minus_lbl, "-");
  lv_obj_center(minus_lbl);
  lv_obj_add_event_cb(minus, on_delta_minus, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* plus = make_round_btn(scr, step_size);
  lv_obj_align(plus, LV_ALIGN_CENTER, 120, -50);
  lv_obj_t* plus_lbl = lv_label_create(plus);
  lv_obj_set_style_text_color(plus_lbl, kColorText, LV_PART_MAIN);
  lv_obj_set_style_text_font(plus_lbl, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_label_set_text(plus_lbl, "+");
  lv_obj_center(plus_lbl);
  lv_obj_add_event_cb(plus, on_delta_plus, LV_EVENT_CLICKED, nullptr);

  // --- Star row: 5 round buttons in a horizontal strip ---
  lv_obj_t* stars_caption = lv_label_create(scr);
  lv_obj_set_style_text_color(stars_caption, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(stars_caption, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(stars_caption, "quality");
  lv_obj_align(stars_caption, LV_ALIGN_CENTER, 0, 40);

  const int32_t star_size = 44;
  const int32_t star_gap  = 12;
  const int32_t row_width = kMaxStars * star_size + (kMaxStars - 1) * star_gap;
  const int32_t row_x0    = kCenter - row_width / 2;
  for (uint8_t i = 0; i < kMaxStars; ++i) {
    lv_obj_t* b = make_round_btn(scr, star_size);
    lv_obj_set_pos(b, row_x0 + i * (star_size + star_gap), kCenter + 70);
    lv_obj_add_event_cb(b, on_star_tap, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    s_star_btns[i] = b;
  }

  // --- Submit button at the bottom ---
  s_submit_btn = lv_button_create(scr);
  lv_obj_set_size(s_submit_btn, 200, 56);
  lv_obj_set_style_radius(s_submit_btn, 28, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_submit_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_submit_btn, 0, LV_PART_MAIN);
  lv_obj_align(s_submit_btn, LV_ALIGN_BOTTOM_MID, 0, -50);
  s_submit_label = lv_label_create(s_submit_btn);
  lv_obj_set_style_text_font(s_submit_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(s_submit_label, "Submit");
  lv_obj_center(s_submit_label);
  lv_obj_add_event_cb(s_submit_btn, on_submit, LV_EVENT_CLICKED, nullptr);

  // --- Saved toast (hidden until a successful append) ---
  s_toast_label = lv_label_create(scr);
  lv_obj_set_style_text_color(s_toast_label, kColorAccent, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_toast_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(s_toast_label, LV_ALIGN_TOP_MID, 0, 110);
  lv_obj_add_flag(s_toast_label, LV_OBJ_FLAG_HIDDEN);

  reset_form();

  display::unlock();
}

}  // namespace espressopost::ui
