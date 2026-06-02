#include "ui_stepper.hpp"

#include <cstdio>

#include "ui_theme.hpp"  // kColorText / kColorMuted / kPostBtn* + COLOR

namespace espressopost::ui {
namespace {

// Stepper disc accent — amber. Its own const (not the menu / post amber) so the
// steppers can drift independently. Shared by every stepper instance.
const lv_color_t kColorStepper = COLOR(0xC88036);

void stepper_minus(lv_event_t* e) {
  auto* st = static_cast<StepperState*>(lv_event_get_user_data(e));
  if (!st->touched)             st->touched = true;
  else if (st->value > st->min) --st->value;
  stepper_refresh(st);
  if (st->on_change) st->on_change();
}

void stepper_plus(lv_event_t* e) {
  auto* st = static_cast<StepperState*>(lv_event_get_user_data(e));
  if (!st->touched)             st->touched = true;
  else if (st->value < st->max) ++st->value;
  stepper_refresh(st);
  if (st->on_change) st->on_change();
}

// Tapping the "--"/value readout while untouched commits the seeded value;
// once set it's display-only — the discs own adjustment from there.
void stepper_value_tap(lv_event_t* e) {
  auto* st = static_cast<StepperState*>(lv_event_get_user_data(e));
  if (st->touched) return;
  st->touched = true;
  stepper_refresh(st);
  if (st->on_change) st->on_change();
}

// One outline disc with a centered glyph — same chrome as the post ✕ button.
lv_obj_t* build_disc(lv_obj_t* parent, const char* glyph, int32_t diam,
                     lv_event_cb_t cb, StepperState* st) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, diam, diam);
  lv_obj_set_style_radius(b, diam / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(b, kColorStepper, LV_PART_MAIN);
  lv_obj_set_style_border_width(b, kPostBtnStroke, LV_PART_MAIN);
  lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_ext_click_area(b, kPostBtnExtClick);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, st);
  lv_obj_t* lbl = lv_label_create(b);
  lv_obj_set_style_text_color(lbl, kColorStepper, LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(lbl, glyph);
  lv_obj_center(lbl);
  return b;
}

}  // namespace

lv_obj_t* build_stepper(lv_obj_t* parent, StepperState* st,
                        const StepperCfg& cfg) {
  // Transparent container, padded so each disc's ext-click area stays within its
  // bounds — a tight parent would otherwise clip taps in the padded zone.
  const int32_t w = 2 * cfg.btn_dx + cfg.btn_diam + 2 * kPostBtnExtClick;
  const int32_t h = cfg.btn_diam + 2 * kPostBtnExtClick;
  lv_obj_t* row = lv_obj_create(parent);
  lv_obj_set_size(row, w, h);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

  st->value_lbl = lv_label_create(row);
  lv_obj_set_style_text_font(st->value_lbl, cfg.value_font, LV_PART_MAIN);
  lv_obj_align(st->value_lbl, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(st->value_lbl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_ext_click_area(st->value_lbl, kPostBtnExtClick);
  lv_obj_add_event_cb(st->value_lbl, stepper_value_tap, LV_EVENT_CLICKED, st);

  lv_obj_t* minus =
      build_disc(row, LV_SYMBOL_MINUS, cfg.btn_diam, stepper_minus, st);
  lv_obj_align(minus, LV_ALIGN_CENTER, -cfg.btn_dx, 0);
  lv_obj_t* plus =
      build_disc(row, LV_SYMBOL_PLUS, cfg.btn_diam, stepper_plus, st);
  lv_obj_align(plus, LV_ALIGN_CENTER, +cfg.btn_dx, 0);

  stepper_refresh(st);
  return row;
}

void stepper_refresh(StepperState* st) {
  if (st == nullptr || st->value_lbl == nullptr) return;
  if (!st->touched) {
    lv_label_set_text(st->value_lbl, "--");
    lv_obj_set_style_text_color(st->value_lbl, kColorMuted, LV_PART_MAIN);
    return;
  }
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%u%c", static_cast<unsigned>(st->value),
                st->unit);
  lv_label_set_text(st->value_lbl, buf);
  lv_obj_set_style_text_color(st->value_lbl, kColorText, LV_PART_MAIN);
}

}  // namespace espressopost::ui
