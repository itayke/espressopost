#include "ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "climate.hpp"
#include "display.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"
#include "model.hpp"
#include "presets.hpp"
#include "rtc.hpp"
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

// Grind range / resolution. Wide enough to cover most grinders' dials (the
// number is whatever the user reads off their hopper); 0.1 step matches the
// finest granularity any common grinder exposes. Clamped to non-negative
// because nobody dials a negative grind setting.
constexpr float kGrindMin  =  0.0f;
constexpr float kGrindMax  = 99.9f;
constexpr float kGrindStep =  0.1f;

constexpr uint8_t kMaxStars = 5;

// AMOLED-friendly muted palette — pure-black background, no max-intensity
// sub-pixels. Same logic as the bringup screen (see git history).
const lv_color_t kColorBg      = LV_COLOR_MAKE(0x00, 0x00, 0x00);
const lv_color_t kColorAccent  = LV_COLOR_MAKE(0xC8, 0x80, 0x36);
const lv_color_t kColorText    = LV_COLOR_MAKE(0xE0, 0xE0, 0xE0);
const lv_color_t kColorMuted   = LV_COLOR_MAKE(0x70, 0x70, 0x70);
const lv_color_t kColorDim     = LV_COLOR_MAKE(0x30, 0x30, 0x30);

lv_obj_t* s_preset_label     = nullptr;
lv_obj_t* s_climate_label    = nullptr;
lv_obj_t* s_delta_label      = nullptr;
lv_obj_t* s_grind_label      = nullptr;
lv_obj_t* s_suggestion_label = nullptr;
lv_obj_t* s_star_btns[kMaxStars] = {};
lv_obj_t* s_submit_btn    = nullptr;
lv_obj_t* s_submit_label  = nullptr;
lv_obj_t* s_toast_label   = nullptr;
lv_timer_t* s_toast_timer = nullptr;

// Cached model output for the currently selected preset. Updated by
// refresh_suggestion() — which runs on every preset cycle and on the climate
// strip's 1 Hz tick. We snapshot it at submit time so the value stored in
// ShotRecord.suggested_grind matches what the user actually saw on screen at
// the moment they pressed Submit (rather than re-running the model after the
// fact and risking a divergent value).
model::Suggestion s_current_suggestion = {std::nanf(""), 0};

// "Has the user touched this field at least once?" — Submit is gated on
// `s_delta_set` and `s_stars_value > 0`. Grind always has a value (it
// auto-populates from the active preset's grind_anchor) so it doesn't gate
// Submit; recording 0 because the anchor is 0 is still meaningful data.
bool   s_delta_set     = false;
int8_t s_delta_value   = 0;
uint8_t s_stars_value  = 0;   // 0 = none picked yet
float   s_grind_value  = 0.0f;

void refresh_grind_label() {
  if (s_grind_label == nullptr) return;
  char buf[24];
  std::snprintf(buf, sizeof(buf), "grind: %.1f", static_cast<double>(s_grind_value));
  lv_label_set_text(s_grind_label, buf);
}

// Re-evaluate the model for the current preset against the latest climate,
// cache the result for the submit path, and update the on-screen suggestion
// line. When the model has nothing useful to say (preset has no shots yet,
// confidence below threshold, β_g near zero, climate not sampled), we keep
// the row visible with a muted "learning..." placeholder rather than hiding
// it entirely — silent disappearance reads as a bug to the user, especially
// after they cycle to an empty preset and the row vanishes without
// explanation.
void refresh_suggestion() {
  if (s_suggestion_label == nullptr) return;
  s_current_suggestion = model::suggest_for_preset(presets::selected_id());
  const bool have_suggestion =
      s_current_suggestion.confidence_pct > 0 &&
      !std::isnan(s_current_suggestion.grind);
  if (have_suggestion) {
    char buf[40];
    std::snprintf(buf, sizeof(buf), "suggested %.1f  \xC2\xB7  %u%%",
                  static_cast<double>(s_current_suggestion.grind),
                  static_cast<unsigned>(s_current_suggestion.confidence_pct));
    lv_label_set_text(s_suggestion_label, buf);
    lv_obj_set_style_text_color(s_suggestion_label, kColorAccent, LV_PART_MAIN);
  } else {
    lv_label_set_text(s_suggestion_label, "learning...");
    lv_obj_set_style_text_color(s_suggestion_label, kColorMuted, LV_PART_MAIN);
  }
  lv_obj_remove_flag(s_suggestion_label, LV_OBJ_FLAG_HIDDEN);
}

void refresh_preset_label() {
  const auto id = presets::selected_id();
  const auto p  = presets::get(id);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%s  \xC2\xB7  target %us", p.name,
                static_cast<unsigned>(p.target_time_s));
  lv_label_set_text(s_preset_label, buf);
}

void on_preset_tap(lv_event_t*) {
  const auto new_id = presets::cycle_selected();
  // Reseed the grind stepper from the new preset's anchor so the next
  // shot defaults to that preset's known-good setting; the user can still
  // step away from it before submitting.
  s_grind_value = presets::get(new_id).grind_anchor;
  refresh_preset_label();
  refresh_grind_label();
  // Each preset has its own model; cycle invalidates the previous suggestion.
  refresh_suggestion();
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
  // Grind resets to the active preset's anchor — most shots will keep the
  // same grind setting across multiple submits, so this saves the user
  // stepping back to the same value every time.
  s_grind_value  = presets::get(presets::selected_id()).grind_anchor;
  refresh_delta_label();
  refresh_grind_label();
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

void on_grind_minus(lv_event_t*) {
  s_grind_value = std::max(kGrindMin, s_grind_value - kGrindStep);
  refresh_grind_label();
}

void on_grind_plus(lv_event_t*) {
  s_grind_value = std::min(kGrindMax, s_grind_value + kGrindStep);
  refresh_grind_label();
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
  rec.preset_id       = presets::selected_id();
  rec.time_delta_s    = s_delta_value;
  rec.quality_stars   = s_stars_value;
  rec.timestamp_us    = esp_timer_get_time();
  rec.rtc_epoch_s     = rtc::epoch_s();      // 0 if RTC not initialized / not yet set
  rec.user_grind      = s_grind_value;
  // Snapshot the suggestion the user saw on screen at submit time. NaN when
  // confidence was 0 (suppressed row) — matches the "no useful guidance" state
  // and lets future analyses tell apart "model said nothing" from "model said
  // exactly N". Refitting happens AFTER the append so this record reflects
  // the model state at decision time, not after this new data point lands.
  rec.suggested_grind = (s_current_suggestion.confidence_pct == 0)
                            ? std::nanf("")
                            : s_current_suggestion.grind;
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
  // New data point — refit and refresh so the next preset cycle / climate
  // tick reflects what we just learned. Cheap on our data volumes; doing it
  // inline keeps the UI deterministic ("save → see updated suggestion") vs
  // deferring to a background task.
  model::refit();
  refresh_suggestion();
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
  // Re-run the suggestion on the climate cadence — model output depends on
  // T/H/P and the user expects the displayed number to track ambient changes
  // without them having to cycle the preset. Cost is tiny (one 4x4 solve at
  // 1 Hz) compared to the climate read itself.
  refresh_suggestion();
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

  // --- Grind stepper: small ±-buttons + inline "grind: X.X" label, slotted
  //     between the time stepper (ends around y=center-15) and the quality
  //     caption (starts at y=center+40). 36 px buttons stay tappable on a
  //     round panel without crowding the time-delta row above it.
  s_grind_label = lv_label_create(scr);
  lv_obj_set_style_text_color(s_grind_label, kColorText, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_grind_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_align(s_grind_label, LV_ALIGN_CENTER, 0, 10);
  refresh_grind_label();

  const int32_t grind_btn = 36;
  lv_obj_t* grind_minus = make_round_btn(scr, grind_btn);
  lv_obj_align(grind_minus, LV_ALIGN_CENTER, -100, 10);
  lv_obj_t* grind_minus_lbl = lv_label_create(grind_minus);
  lv_obj_set_style_text_color(grind_minus_lbl, kColorText, LV_PART_MAIN);
  lv_obj_set_style_text_font(grind_minus_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(grind_minus_lbl, "-");
  lv_obj_center(grind_minus_lbl);
  lv_obj_add_event_cb(grind_minus, on_grind_minus, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* grind_plus = make_round_btn(scr, grind_btn);
  lv_obj_align(grind_plus, LV_ALIGN_CENTER, 100, 10);
  lv_obj_t* grind_plus_lbl = lv_label_create(grind_plus);
  lv_obj_set_style_text_color(grind_plus_lbl, kColorText, LV_PART_MAIN);
  lv_obj_set_style_text_font(grind_plus_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(grind_plus_lbl, "+");
  lv_obj_center(grind_plus_lbl);
  lv_obj_add_event_cb(grind_plus, on_grind_plus, LV_EVENT_CLICKED, nullptr);

  // --- Model suggestion: small muted line sandwiched between the grind row
  //     and the quality caption. Hidden by default (refresh_suggestion will
  //     toggle visibility based on confidence). Accent color so users notice
  //     when it appears, but tiny so it never competes with the primary
  //     input controls.
  s_suggestion_label = lv_label_create(scr);
  lv_obj_set_style_text_color(s_suggestion_label, kColorAccent, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_suggestion_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(s_suggestion_label, LV_ALIGN_CENTER, 0, 32);
  lv_obj_add_flag(s_suggestion_label, LV_OBJ_FLAG_HIDDEN);

  // --- Star row: 5 round buttons in a horizontal strip. Caption and buttons
  //     bumped 18 px south of their original positions to make room for the
  //     model's suggestion line above (slotted between grind row and stars).
  lv_obj_t* stars_caption = lv_label_create(scr);
  lv_obj_set_style_text_color(stars_caption, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(stars_caption, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(stars_caption, "quality");
  lv_obj_align(stars_caption, LV_ALIGN_CENTER, 0, 58);

  const int32_t star_size = 44;
  const int32_t star_gap  = 12;
  const int32_t row_width = kMaxStars * star_size + (kMaxStars - 1) * star_gap;
  const int32_t row_x0    = kCenter - row_width / 2;
  for (uint8_t i = 0; i < kMaxStars; ++i) {
    lv_obj_t* b = make_round_btn(scr, star_size);
    lv_obj_set_pos(b, row_x0 + i * (star_size + star_gap), kCenter + 88);
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
  // First paint of the suggestion line. If climate hasn't sampled yet or the
  // model can't speak for this preset, the row stays hidden — the 1 Hz
  // climate timer will reveal it as soon as data arrives.
  refresh_suggestion();

  display::unlock();
}

}  // namespace espressopost::ui
