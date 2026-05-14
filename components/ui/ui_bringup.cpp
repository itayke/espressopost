#include "ui.hpp"

#include <cstdio>

#include "display.hpp"
#include "lvgl.h"

namespace espressopost::ui {
namespace {

constexpr int32_t kArcMin = 0;
constexpr int32_t kArcMax = 100;
constexpr int32_t kArcInitial = 50;

// Pure-black background is "off pixels" on AMOLED — lower power, no burn-in.
// Foreground colors are intentionally muted (not maximum-intensity) because
// max-intensity sub-pixels are exactly what burns in fastest.
// `static const` rather than `constexpr` because lv_color_t's constructor
// isn't constexpr across all LVGL build configs.
const lv_color_t kColorBg     = LV_COLOR_MAKE(0x00, 0x00, 0x00);
const lv_color_t kColorAccent = LV_COLOR_MAKE(0xC8, 0x80, 0x36);  // muted espresso amber
const lv_color_t kColorText   = LV_COLOR_MAKE(0xE0, 0xE0, 0xE0);

lv_obj_t* s_value_label = nullptr;

void on_arc_value_change(lv_event_t* e) {
  auto* arc = static_cast<lv_obj_t*>(lv_event_get_target(e));
  const int32_t v = lv_arc_get_value(arc);
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%ld", static_cast<long>(v));
  lv_label_set_text(s_value_label, buf);
}

}  // namespace

void start_bringup() {
  if (!display::lock()) return;

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, kColorBg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

  // Touch arc around the rim. Round display = full 360° works visually, but
  // arc widgets behave better with a small gap so the start/end is obvious.
  // 30° gap at the bottom: start 120°, end 60° (going clockwise through 0).
  lv_obj_t* arc = lv_arc_create(scr);
  const int32_t arc_size = 460;  // 466 minus a 3 px margin
  lv_obj_set_size(arc, arc_size, arc_size);
  lv_obj_center(arc);

  lv_arc_set_range(arc, kArcMin, kArcMax);
  // Angle convention: 0° at 3 o'clock, increasing clockwise. Visible arc
  // sweeps from 135° (lower-left) clockwise through 180°→270°→0° to 45°
  // (upper-right); the gap is centered at 90° (the bottom — clear of the
  // user's grip when held in the right hand).
  lv_arc_set_bg_angles(arc, 135, 45);
  lv_arc_set_value(arc, kArcInitial);

  // Track & knob styling. Width = 18 px gives a comfortable wet-finger target
  // around the rim without crowding the centered number.
  lv_obj_set_style_arc_width(arc, 18, LV_PART_MAIN);
  lv_obj_set_style_arc_color(arc, LV_COLOR_MAKE(0x20, 0x20, 0x20), LV_PART_MAIN);
  lv_obj_set_style_arc_width(arc, 18, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(arc, kColorAccent, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(arc, kColorAccent, LV_PART_KNOB);
  lv_obj_set_style_bg_opa(arc, LV_OPA_COVER, LV_PART_KNOB);
  lv_obj_set_style_pad_all(arc, 4, LV_PART_KNOB);

  lv_obj_add_event_cb(arc, on_arc_value_change, LV_EVENT_VALUE_CHANGED, nullptr);

  // Centered numeric label.
  s_value_label = lv_label_create(scr);
  lv_obj_center(s_value_label);
  static lv_style_t big_num_style;
  lv_style_init(&big_num_style);
  lv_style_set_text_color(&big_num_style, kColorText);
  lv_style_set_text_font(&big_num_style, &lv_font_montserrat_48);
  lv_obj_add_style(s_value_label, &big_num_style, LV_PART_MAIN);

  char buf[8];
  std::snprintf(buf, sizeof(buf), "%ld", static_cast<long>(kArcInitial));
  lv_label_set_text(s_value_label, buf);

  display::unlock();
}

}  // namespace espressopost::ui
