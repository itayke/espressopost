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

// ---------------------------------------------------------------------------
// Geometry — round 466×466 AMOLED. The outer rim hosts the grind ring;
// everything else stacks in the central disk.
// ---------------------------------------------------------------------------
constexpr int32_t kScreen          = 466;
constexpr int32_t kCenter          = kScreen / 2;
constexpr int32_t kDotRingRadius   = 215;  // rim dot strip (swipe assistant)
constexpr int32_t kCursorRadius    = 195;  // static cursor + predicted arrow, just inside dot ring
constexpr int32_t kRingInnerEdge   = 155;  // press inside this = center widget, not ring drag
constexpr int32_t kBigDotRadius    = 5;    // every-5-units dot
constexpr int32_t kSmallDotRadius  = 2;    // 0.1-step dot
constexpr float   kArcHalfDeg      = 30.0f;  // dot strip is 60° wide centered at 6 o'clock
constexpr float   kPressBufferDeg  = 15.0f;  // press-to-start-drag tolerance outside the arc

// ---------------------------------------------------------------------------
// Grind dial — 0..30 in 0.1 steps. The rim isn't a value display; it's a
// swipe-feedback dot strip in a 60° window at the bottom of the screen.
// Pitch (degrees per integer unit) is decoupled from the dial range — the
// "270° around the ring with a 90° gap" thinking was an artefact of the
// rotating-bezel design we abandoned. The dot strip just shows the
// fractional position around the cursor: at v=5.1 the integer dot sits 3°
// off the cursor; at v=4.1 it's in the *same* place. The actual value
// comes from the big text above the cursor. This makes pitch a pure feel
// knob — bigger = more deliberate dial, smaller = quicker.
//
// Older firmware that stored a value above 30 gets clamped on UI load; the
// raw NVS float is left alone, only what we show/persist next is bounded.
// ---------------------------------------------------------------------------
constexpr float kGrindMin       =  0.0f;
constexpr float kGrindMax       = 30.0f;
constexpr float kGrindStep      =  0.1f;
constexpr float kDegPerUnit     = 30.0f;          // °/grind unit — feel knob
constexpr float kSubDotPitchDeg = kDegPerUnit / 10.0f;  // 0.1 step = 3°

// Time-delta range. Wider than realistic so a long-pull-and-channel doesn't
// clip; the model downweights outliers via the quality field anyway.
constexpr int8_t kDeltaMin  = -30;
constexpr int8_t kDeltaMax  =  30;
constexpr int8_t kDeltaStep =   1;

constexpr uint8_t kMaxStars = 5;

// ---------------------------------------------------------------------------
// AMOLED-friendly muted palette — pure-black background, no max-intensity
// sub-pixels (saves burn-in). Same logic as the bringup screen.
// ---------------------------------------------------------------------------
const lv_color_t kColorBg     = LV_COLOR_MAKE(0x00, 0x00, 0x00);
const lv_color_t kColorAccent = LV_COLOR_MAKE(0xC8, 0x80, 0x36);
const lv_color_t kColorText   = LV_COLOR_MAKE(0xE0, 0xE0, 0xE0);
const lv_color_t kColorMuted  = LV_COLOR_MAKE(0x70, 0x70, 0x70);
const lv_color_t kColorDim    = LV_COLOR_MAKE(0x30, 0x30, 0x30);
const lv_color_t kColorGreen  = LV_COLOR_MAKE(0x40, 0xB0, 0x60);
const lv_color_t kColorOrange = LV_COLOR_MAKE(0xD8, 0x90, 0x30);
const lv_color_t kColorRed    = LV_COLOR_MAKE(0xC8, 0x40, 0x40);

// ---------------------------------------------------------------------------
// UI modes. The ring + cursor + predicted arrow stay live across both;
// only the center widgets swap.
// ---------------------------------------------------------------------------
enum class Mode { Idle, Post };
Mode s_mode = Mode::Idle;

// ---------------------------------------------------------------------------
// Widget handles. Grouped by visual role; null until start_report() builds them.
// ---------------------------------------------------------------------------
// Ring (always visible across Idle + Post):
lv_obj_t* s_ring_overlay      = nullptr;  // transparent full-screen rim-gesture catcher
lv_obj_t* s_dot_strip         = nullptr;  // tiny custom-drawn widget for the 30° dot window
lv_obj_t* s_static_cursor     = nullptr;
lv_obj_t* s_predicted_arrow   = nullptr;
lv_obj_t* s_grind_value_label = nullptr;  // big "5.2" above the cursor — visible in both modes

// Idle group:
lv_obj_t* s_idle_group          = nullptr;
lv_obj_t* s_preset_label        = nullptr;
lv_obj_t* s_climate_label       = nullptr;
lv_obj_t* s_post_btn            = nullptr;

// Post group:
lv_obj_t* s_post_group          = nullptr;
lv_obj_t* s_delta_label         = nullptr;
lv_obj_t* s_star_btns[kMaxStars] = {};
lv_obj_t* s_submit_btn          = nullptr;
lv_obj_t* s_submit_label        = nullptr;
lv_obj_t* s_cancel_btn          = nullptr;

// Toast (transient):
lv_obj_t* s_toast_label         = nullptr;
lv_timer_t* s_toast_timer       = nullptr;

// ---------------------------------------------------------------------------
// Form / model state.
// ---------------------------------------------------------------------------
model::Suggestion s_current_suggestion = {std::nanf(""), 0};

bool    s_delta_set    = false;
int8_t  s_delta_value  = 0;
uint8_t s_stars_value  = 0;
float   s_grind_value  = 0.0f;

// Ring drag state — captured at PRESSED, advanced on PRESSING, persisted on
// RELEASED. s_ring_dragging gates PRESSING/RELEASED so a press that started
// in the center (and isn't a ring drag) doesn't accidentally rotate the ring.
bool  s_ring_dragging       = false;
float s_drag_last_angle     = 0.0f;  // degrees, CW from 12 o'clock
// Un-snapped grind value tracked during a drag. The displayed s_grind_value
// rounds to 0.1; this one tracks the finger exactly so slow motion under
// the snap quantum still accumulates across frames instead of being
// discarded. Seeded from s_grind_value on PRESSED.
float s_grind_value_raw     = 0.0f;

// ---------------------------------------------------------------------------
// Math helpers.
// ---------------------------------------------------------------------------
constexpr float kPi = 3.14159265f;

// Screen angle (CW from 12 o'clock, degrees) where value `v_i` is decaled
// given the current ring position `v_cur`. The convention is "higher values
// CCW from cursor" so a CW finger drag = CW ring rotation = HIGHER value at
// cursor — i.e. dialing UP feels like the bezel spins WITH the finger.
float value_angle_deg(float v_i, float v_cur) {
  return 180.0f - (v_i - v_cur) * kDegPerUnit;
}

void polar_to_screen(float angle_deg, int32_t radius, int32_t* out_x, int32_t* out_y) {
  const float rad = angle_deg * (kPi / 180.0f);
  *out_x = kCenter + static_cast<int32_t>(std::lround(radius * std::sin(rad)));
  *out_y = kCenter - static_cast<int32_t>(std::lround(radius * std::cos(rad)));
}

// Confidence-tier color for the predicted arrow. Caller is responsible for
// hiding the arrow when confidence_pct <= 30 (this returns red for that band
// just for completeness; nothing should ever render it).
lv_color_t confidence_color(uint8_t pct) {
  if (pct > 80) return kColorGreen;
  if (pct > 50) return kColorOrange;
  return kColorRed;
}

bool predicted_visible(const model::Suggestion& s) {
  return s.confidence_pct > 30 &&
         !std::isnan(s.grind) &&
         s.grind >= kGrindMin &&
         s.grind <= kGrindMax;
}

// ---------------------------------------------------------------------------
// Arrow widgets (cursor + predicted indicator). The original implementation
// used a label with U+25BC "▼", but Montserrat doesn't ship the geometric-
// shapes range so the glyph fell back to a missing-glyph rectangle. We draw
// the triangle ourselves via lv_draw_triangle and store per-arrow state
// (angle + color) in the widget's user_data so a single DRAW_MAIN handler
// serves both the static cursor and the rotating predicted indicator.
// ---------------------------------------------------------------------------
struct ArrowState {
  float      angle_deg;  // 180° = tip pointing straight down (outward at 6 o'clock)
  lv_color_t color;
};
ArrowState s_cursor_arrow_state    = {180.0f, LV_COLOR_MAKE(0xE0, 0xE0, 0xE0)};
ArrowState s_predicted_arrow_state = {180.0f, LV_COLOR_MAKE(0xC8, 0x40, 0x40)};

constexpr int32_t kArrowHalfBase = 7;   // base half-width (so base = 14 px)
constexpr int32_t kArrowHeight   = 14;  // tip-to-base distance

void draw_arrow_event(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  if (layer == nullptr) return;
  auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
  auto* state = static_cast<ArrowState*>(lv_obj_get_user_data(obj));
  if (state == nullptr) return;

  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  const float cx = (coords.x1 + coords.x2) * 0.5f;
  const float cy = (coords.y1 + coords.y2) * 0.5f;

  // Default orientation: tip at (0, +h/2), base from (−w/2, −h/2) to
  // (+w/2, −h/2) — so the triangle points straight DOWN (screen +y).
  // To make the tip point outward at any rim angle θ, rotate CW by
  // (θ − 180°). Rotation matrix in screen y-down coords is the same as
  // math convention: (x, y) → (x·cos α − y·sin α, x·sin α + y·cos α).
  const float rad = (state->angle_deg - 180.0f) * (kPi / 180.0f);
  const float cs = std::cos(rad);
  const float sn = std::sin(rad);
  auto rot = [&](float lx, float ly) {
    lv_point_precise_t p;
    p.x = static_cast<lv_value_precise_t>(cx + lx * cs - ly * sn);
    p.y = static_cast<lv_value_precise_t>(cy + lx * sn + ly * cs);
    return p;
  };

  lv_draw_triangle_dsc_t dsc;
  lv_draw_triangle_dsc_init(&dsc);
  dsc.color = state->color;
  dsc.opa   = LV_OPA_COVER;
  dsc.p[0] = rot(0.0f,                  +kArrowHeight   * 0.5f);
  dsc.p[1] = rot(-kArrowHalfBase * 1.0f, -kArrowHeight  * 0.5f);
  dsc.p[2] = rot(+kArrowHalfBase * 1.0f, -kArrowHeight  * 0.5f);
  lv_draw_triangle(layer, &dsc);
}

// ---------------------------------------------------------------------------
// Dot strip — LV_EVENT_DRAW_MAIN handler. The dial value snaps to 0.1, so
// the cursor is always sitting exactly on a sub-step's dot. We emit dots
// outward from the cursor at multiples of kSubDotPitchDeg, marking the ones
// whose distance-from-cursor is a full integer in grind units as big.
//
// Crucially, dot positions only depend on s_grind_value mod 0.1 (which is
// always 0 after snap) and on which sub-step the cursor sits on (for the
// "is integer" check). The rim therefore looks identical at v=5.1 and
// v=4.1 — exactly the swipe-feedback semantics we want: the rim is just
// "your finger is doing something," the actual number lives in the text
// above the cursor.
// ---------------------------------------------------------------------------
void draw_dot_strip(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  if (layer == nullptr) return;

  // Cursor's sub-step index in absolute terms (51 for v=5.1). Only used to
  // decide which neighbours are integer-aligned — never for positioning.
  const int32_t center_substep =
      static_cast<int32_t>(std::lround(s_grind_value * 10.0f));
  const int32_t max_n =
      static_cast<int32_t>(std::ceil(kArcHalfDeg / kSubDotPitchDeg)) + 1;

  lv_draw_rect_dsc_t big_dsc;
  lv_draw_rect_dsc_init(&big_dsc);
  big_dsc.bg_color = kColorText;
  big_dsc.bg_opa   = LV_OPA_COVER;
  big_dsc.radius   = LV_RADIUS_CIRCLE;

  lv_draw_rect_dsc_t small_dsc;
  lv_draw_rect_dsc_init(&small_dsc);
  small_dsc.bg_color = kColorMuted;
  small_dsc.bg_opa   = LV_OPA_COVER;
  small_dsc.radius   = LV_RADIUS_CIRCLE;

  // Sign: positive n = higher grind value than cursor = CCW of cursor in
  // screen-angle convention (lower angle). Negative n = lower value = CW.
  for (int32_t n = -max_n; n <= max_n; ++n) {
    const float angle_offset = -n * kSubDotPitchDeg;
    if (std::fabs(angle_offset) > kArcHalfDeg + 0.5f) continue;
    const float angle_deg = 180.0f + angle_offset;
    int32_t x, y;
    polar_to_screen(angle_deg, kDotRingRadius, &x, &y);
    const bool is_big = ((center_substep + n) % 10 == 0);
    const int32_t r = is_big ? kBigDotRadius : kSmallDotRadius;
    lv_area_t a = {x - r, y - r, x + r, y + r};
    lv_draw_rect(layer, is_big ? &big_dsc : &small_dsc, &a);
  }
}

// ---------------------------------------------------------------------------
// Ring refresh — invalidate just the bottom-arc dot widget. The dot strip is
// tiny (~140×35 px); LVGL only repaints that rectangle, not the screen. The
// custom DRAW_MAIN handler computes which dots are visible at the current
// s_grind_value and paints them — no font rendering, no widget tree churn,
// no PSRAM-bound rotation. The predicted arrow still rides the full rim at
// its model-suggested angle (one small widget, repositioned per frame).
// ---------------------------------------------------------------------------
void refresh_predicted_arrow();

void refresh_ring() {
  if (s_dot_strip != nullptr) {
    lv_obj_invalidate(s_dot_strip);
  }
  refresh_predicted_arrow();
}

// ---------------------------------------------------------------------------
// Predicted arrow — same triangle glyph as the static cursor, positioned at
// the predicted grind's angle on the ring, rotated to point outward, colored
// by confidence tier. Hidden entirely when confidence ≤ 30 (the user sees an
// empty rim, which IS the signal: "model has nothing useful to say").
// ---------------------------------------------------------------------------
void refresh_predicted_arrow() {
  if (s_predicted_arrow == nullptr) return;
  if (!predicted_visible(s_current_suggestion)) {
    lv_obj_add_flag(s_predicted_arrow, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  const float v_p   = s_current_suggestion.grind;
  const float angle = value_angle_deg(v_p, s_grind_value);
  int32_t x, y;
  polar_to_screen(angle, kCursorRadius, &x, &y);
  const int32_t w = lv_obj_get_width(s_predicted_arrow);
  const int32_t h = lv_obj_get_height(s_predicted_arrow);
  lv_obj_set_pos(s_predicted_arrow, x - w / 2, y - h / 2);

  // Push the new angle + color into the arrow's state and invalidate so the
  // DRAW_MAIN handler picks them up. No transforms / no fonts in play; the
  // triangle is rasterized fresh each frame from these two scalars.
  s_predicted_arrow_state.angle_deg = angle;
  s_predicted_arrow_state.color =
      confidence_color(s_current_suggestion.confidence_pct);
  lv_obj_invalidate(s_predicted_arrow);
  lv_obj_remove_flag(s_predicted_arrow, LV_OBJ_FLAG_HIDDEN);
}

// ---------------------------------------------------------------------------
// Center-widget refreshers.
// ---------------------------------------------------------------------------
void refresh_grind_value_label() {
  if (s_grind_value_label == nullptr) return;
  char buf[12];
  std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(s_grind_value));
  lv_label_set_text(s_grind_value_label, buf);
}

void refresh_preset_label() {
  if (s_preset_label == nullptr) return;
  const auto id = presets::selected_id();
  const auto p  = presets::get(id);
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%s  \xC2\xB7  target %us", p.name,
                static_cast<unsigned>(p.target_time_s));
  lv_label_set_text(s_preset_label, buf);
}

void refresh_delta_label() {
  if (s_delta_label == nullptr) return;
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
    if (s_star_btns[i] == nullptr) continue;
    const bool lit = (i < s_stars_value);
    lv_obj_set_style_bg_color(s_star_btns[i], lit ? kColorAccent : kColorDim,
                              LV_PART_MAIN);
  }
}

void refresh_submit_enabled() {
  if (s_submit_btn == nullptr) return;
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

// Re-evaluate the model and update the predicted-arrow display. Runs on
// preset cycle and the 1 Hz climate tick.
void refresh_suggestion() {
  s_current_suggestion = model::suggest_for_preset(presets::selected_id());
  const bool have_suggestion =
      s_current_suggestion.confidence_pct > 0 &&
      !std::isnan(s_current_suggestion.grind);

  // DIAGNOSTIC — paired with the boot-time shot dump in storage::init().
  // Fires on three triggers so we see drift without flooding the monitor:
  // (a) first time a valid suggestion materializes, (b) when grind/conf
  // output changes, (c) every ~60s as a drift heartbeat. The numeric
  // confidence stays on disk and in the log even though the UI no longer
  // shows it — the color-tiered arrow encodes only `<30 hidden / >30 / >50 /
  // >80`, so the log is the only place to see the underlying math.
  static bool    s_prev_valid       = false;
  static float   s_prev_grind       = 0.0f;
  static uint8_t s_prev_conf        = 0;
  static int     s_ticks_since_log  = 60;
  const bool valid_changed  = have_suggestion != s_prev_valid;
  const bool output_changed = have_suggestion &&
      (s_current_suggestion.grind != s_prev_grind ||
       s_current_suggestion.confidence_pct != s_prev_conf);
  const bool periodic = s_ticks_since_log >= 60;
  if (valid_changed || output_changed || periodic) {
    const climate::Reading r = climate::latest();
    if (r.timestamp_us != 0) {
      if (have_suggestion) {
        ESP_LOGI(kTag,
                 "state: T=%.2f H=%.2f P=%.2f preset=%u → grind=%.2f conf=%u%%",
                 static_cast<double>(r.temp_c),
                 static_cast<double>(r.humidity_pct),
                 static_cast<double>(r.pressure_hpa),
                 static_cast<unsigned>(presets::selected_id()),
                 static_cast<double>(s_current_suggestion.grind),
                 static_cast<unsigned>(s_current_suggestion.confidence_pct));
      } else {
        ESP_LOGI(kTag,
                 "state: T=%.2f H=%.2f P=%.2f preset=%u → (suggestion hidden)",
                 static_cast<double>(r.temp_c),
                 static_cast<double>(r.humidity_pct),
                 static_cast<double>(r.pressure_hpa),
                 static_cast<unsigned>(presets::selected_id()));
      }
      s_prev_valid      = have_suggestion;
      s_prev_grind      = s_current_suggestion.grind;
      s_prev_conf       = s_current_suggestion.confidence_pct;
      s_ticks_since_log = 0;
    }
  } else {
    ++s_ticks_since_log;
  }
  refresh_predicted_arrow();
}

// ---------------------------------------------------------------------------
// Mode transitions. Hide the inactive group entirely so its widgets don't
// receive stray touches; the ring + cursor + predicted arrow stay visible in
// both modes so the user can keep dialing while filling out the post form.
// ---------------------------------------------------------------------------
void apply_mode() {
  if (s_mode == Mode::Idle) {
    lv_obj_remove_flag(s_idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_post_group, LV_OBJ_FLAG_HIDDEN);
    if (s_grind_value_label) lv_obj_remove_flag(s_grind_value_label, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_post_group, LV_OBJ_FLAG_HIDDEN);
    // Value text is hidden in Post — the central disk is busy with the
    // delta stepper, stars, and Submit. The rim dots still scroll under
    // touch so the user has feedback that dragging works.
    if (s_grind_value_label) lv_obj_add_flag(s_grind_value_label, LV_OBJ_FLAG_HIDDEN);
  }
}

void enter_idle() {
  s_mode = Mode::Idle;
  // Reset the post form so re-entering doesn't carry over a stale delta/stars
  // from the previous attempt. Grind survives — the user already dialed it
  // and the ring/cursor are still showing the last position.
  s_delta_set    = false;
  s_delta_value  = 0;
  s_stars_value  = 0;
  refresh_delta_label();
  refresh_stars();
  refresh_submit_enabled();
  apply_mode();
}

void enter_post() {
  s_mode = Mode::Post;
  apply_mode();
}

// ---------------------------------------------------------------------------
// Toast.
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Event handlers.
// ---------------------------------------------------------------------------
void on_preset_tap(lv_event_t*) {
  if (s_mode != Mode::Idle) return;  // belt-and-suspenders: hidden in post mode
  const auto new_id = presets::cycle_selected();
  s_grind_value = std::clamp(presets::last_grind(new_id), kGrindMin, kGrindMax);
  refresh_preset_label();
  refresh_grind_value_label();
  refresh_suggestion();
  refresh_ring();
}

void on_post_tap(lv_event_t*) {
  enter_post();
}

void on_cancel_tap(lv_event_t*) {
  enter_idle();
}

void on_delta_minus(lv_event_t*) {
  if (!s_delta_set) { s_delta_set = true; s_delta_value = 0; }
  s_delta_value = std::max<int8_t>(kDeltaMin,
      static_cast<int8_t>(s_delta_value - kDeltaStep));
  refresh_delta_label();
  refresh_submit_enabled();
}

void on_delta_plus(lv_event_t*) {
  if (!s_delta_set) { s_delta_set = true; s_delta_value = 0; }
  s_delta_value = std::min<int8_t>(kDeltaMax,
      static_cast<int8_t>(s_delta_value + kDeltaStep));
  refresh_delta_label();
  refresh_submit_enabled();
}

void on_star_tap(lv_event_t* e) {
  const auto idx = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
  if (s_stars_value == idx + 1) s_stars_value = static_cast<uint8_t>(idx);
  else                          s_stars_value = static_cast<uint8_t>(idx + 1);
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
  rec.rtc_epoch_s     = rtc::epoch_s();
  rec.user_grind      = s_grind_value;
  // Snapshot the suggestion the user saw at submit time. NaN when the arrow
  // was hidden (confidence ≤ 30) — matches "model said nothing" and lets
  // future analyses distinguish that from "model said exactly N". Refit runs
  // AFTER append so this record reflects the model state at decision time.
  rec.suggested_grind = predicted_visible(s_current_suggestion)
                            ? s_current_suggestion.grind
                            : std::nanf("");
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
  std::snprintf(buf, sizeof(buf), "Saved #%u",
                static_cast<unsigned>(storage::shot_count()));
  show_toast(buf);
  enter_idle();
  // New data point — refit and refresh so the arrow reflects what we just
  // learned the next climate tick (cheap on our data volumes; inline keeps
  // UI deterministic vs deferring to a background task).
  model::refit();
  refresh_suggestion();
  refresh_ring();
}

// Ring drag. We can't use LVGL's high-level gestures (those fire only on
// swipe-up/down/left/right). Instead we attach PRESSED/PRESSING/RELEASED to
// the full-screen overlay and convert the touch's angle-from-center into
// grind value deltas. The center widgets sit z-above the overlay so they
// take their own touches; press-events inside the inner radius are also
// rejected here as a second safety net.
void on_ring_event(lv_event_t* e) {
  const auto code = lv_event_get_code(e);
  lv_indev_t* indev = lv_indev_active();
  if (indev == nullptr) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);
  const float dx = static_cast<float>(p.x - kCenter);
  const float dy = static_cast<float>(p.y - kCenter);
  const float r  = std::sqrt(dx * dx + dy * dy);

  switch (code) {
    case LV_EVENT_PRESSED: {
      if (r < kRingInnerEdge) {
        s_ring_dragging = false;  // press inside center → not a ring drag
        return;
      }
      // Restrict the start of a drag to the dot strip arc (plus a buffer so
      // the user doesn't need pixel-perfect aim). Once dragging, the finger
      // can wander anywhere — that's still tracked via angular delta.
      // Touch angle in [-180, 180], 0 = top; "distance to 6 o'clock" is
      // (180° − |angle|).
      const float touch_angle =
          std::atan2(dx, -dy) * (180.0f / kPi);
      const float dist_from_bottom = 180.0f - std::fabs(touch_angle);
      if (dist_from_bottom > kArcHalfDeg + kPressBufferDeg) {
        s_ring_dragging = false;  // press is somewhere on the silent rim
        return;
      }
      s_ring_dragging   = true;
      s_drag_last_angle = touch_angle;
      // Seed the raw value at the snapped one so sub-snap motion can
      // accumulate from there. Without this we'd lose any partial-step
      // motion the user has built up in a previous drag.
      s_grind_value_raw = s_grind_value;
      break;
    }
    case LV_EVENT_PRESSING: {
      if (!s_ring_dragging) return;
      const float a = std::atan2(dx, -dy) * (180.0f / kPi);
      float delta = a - s_drag_last_angle;
      if (delta >  180.0f) delta -= 360.0f;
      if (delta < -180.0f) delta += 360.0f;
      s_drag_last_angle = a;
      // CW finger motion (positive delta) → ring rotates WITH finger → cursor
      // sees higher value → grind UP. Matches the bezel-spins-with-finger
      // mental model.
      //
      // Apply the delta to the un-snapped raw value, then snap for display.
      // Slow finger motion that produces <0.05 grind units per frame would
      // be discarded by snap-then-store, but the raw accumulator picks it
      // up across frames and tips over the snap boundary once enough motion
      // has built up.
      const float dv = delta / kDegPerUnit;
      s_grind_value_raw =
          std::clamp(s_grind_value_raw + dv, kGrindMin, kGrindMax);
      const float new_value = std::round(s_grind_value_raw * 10.0f) / 10.0f;
      if (new_value != s_grind_value) {
        s_grind_value = new_value;
        refresh_ring();
        refresh_grind_value_label();
      }
      break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: {
      if (s_ring_dragging) {
        s_ring_dragging = false;
        presets::set_last_grind(presets::selected_id(), s_grind_value);
      }
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Climate strip update — 1 Hz. Also drives suggestion refresh on the same
// cadence so the arrow tracks ambient changes without the user touching
// anything.
// ---------------------------------------------------------------------------
void update_climate_strip(lv_timer_t*) {
  if (s_climate_label != nullptr) {
    const climate::Reading r = climate::latest();
    if (r.timestamp_us == 0) {
      lv_label_set_text(s_climate_label, "P --  H --  T --");
    } else {
      const float p_inhg = climate::hpa_to_inhg(r.pressure_hpa);
      const float t_f    = climate::c_to_f(r.temp_c);
      char buf[48];
      std::snprintf(buf, sizeof(buf), "P %.2finHg  H %.0f%%  T %.1f\xC2\xB0""F",
                    p_inhg, r.humidity_pct, t_f);
      lv_label_set_text(s_climate_label, buf);
    }
  }
  refresh_suggestion();
}

// ---------------------------------------------------------------------------
// Widget factories.
// ---------------------------------------------------------------------------
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

lv_obj_t* make_arrow(lv_obj_t* parent, ArrowState* state) {
  lv_obj_t* a = lv_obj_create(parent);
  // Widget needs to be big enough to contain the triangle at any rotation;
  // the diagonal of a kArrowHalfBase·2 × kArrowHeight isoceles triangle
  // bounds at roughly 1.5× the longer side, so 28×28 leaves comfortable
  // headroom for the predicted indicator's full rotation range.
  lv_obj_set_size(a, 28, 28);
  lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(a, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(a, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(a, 0, LV_PART_MAIN);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(a, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(a, state);
  lv_obj_add_event_cb(a, draw_arrow_event, LV_EVENT_DRAW_MAIN, nullptr);
  return a;
}

// ---------------------------------------------------------------------------
// Group builders.
// ---------------------------------------------------------------------------
void build_ring(lv_obj_t* scr) {
  // Transparent full-screen overlay catches rim touches. Sits BEHIND the
  // center widget groups so its handler only fires when the press lands
  // outside any center widget.
  s_ring_overlay = lv_obj_create(scr);
  lv_obj_set_size(s_ring_overlay, kScreen, kScreen);
  lv_obj_set_pos(s_ring_overlay, 0, 0);
  lv_obj_set_style_bg_opa(s_ring_overlay, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_ring_overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_ring_overlay, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_ring_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_ring_overlay, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(s_ring_overlay, on_ring_event, LV_EVENT_PRESSED, nullptr);
  lv_obj_add_event_cb(s_ring_overlay, on_ring_event, LV_EVENT_PRESSING, nullptr);
  lv_obj_add_event_cb(s_ring_overlay, on_ring_event, LV_EVENT_RELEASED, nullptr);
  lv_obj_add_event_cb(s_ring_overlay, on_ring_event, LV_EVENT_PRESS_LOST, nullptr);

  // Dot strip — small custom-drawn widget covering only the bottom 30° arc.
  // On every drag, LVGL invalidates THIS widget's tiny bounding box (~140×35
  // px), not the screen. The draw callback computes which 0.1-step positions
  // currently fall inside the arc and renders them as filled circles (big
  // for integer values, small otherwise). No font rendering, no PSRAM-bound
  // image rotation — the per-frame cost drops to a few hundred pixels of
  // fill work.
  //
  // Bounding box math: dots sit at radius kDotRingRadius (215). The 60° arc
  // centered at 6 o'clock spans screen-angles [150°, 210°]. The widest x
  // excursion is r·sin(30°) ≈ 108 from center; the highest dot is at
  // r·cos(30°) ≈ 186 below center, the lowest at r·cos(0°) = r (at exactly
  // 6 o'clock). Pad ±10 for the big-dot radius and a couple of safety pixels.
  {
    constexpr int32_t kHalfW = 125;
    constexpr int32_t kStripH = 42;
    const int32_t y0 = kCenter + kDotRingRadius - kStripH + 10;
    s_dot_strip = lv_obj_create(scr);
    lv_obj_set_size(s_dot_strip, kHalfW * 2, kStripH);
    lv_obj_set_pos(s_dot_strip, kCenter - kHalfW, y0);
    lv_obj_set_style_bg_opa(s_dot_strip, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_dot_strip, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_dot_strip, 0, LV_PART_MAIN);
    lv_obj_clear_flag(s_dot_strip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_dot_strip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_dot_strip, draw_dot_strip, LV_EVENT_DRAW_MAIN,
                        nullptr);
  }

  // Static cursor at 6 o'clock, tip pointing OUTWARD (down) at the dot
  // strip. Drawn as a filled triangle (see draw_arrow_event); state stays
  // fixed at 180° / kColorText, so no refresh needed after init.
  s_cursor_arrow_state = {180.0f, kColorText};
  s_static_cursor = make_arrow(scr, &s_cursor_arrow_state);
  {
    int32_t cx, cy;
    polar_to_screen(180.0f, kCursorRadius, &cx, &cy);
    const int32_t w = lv_obj_get_width(s_static_cursor);
    const int32_t h = lv_obj_get_height(s_static_cursor);
    lv_obj_set_pos(s_static_cursor, cx - w / 2, cy - h / 2);
  }

  // Predicted arrow — created hidden; refresh_predicted_arrow() positions
  // it on the rim at the suggested grind's angle and recolors per
  // confidence tier. Same orbit as the static cursor so the two visually
  // stack when the user has dialed exactly the suggested value.
  s_predicted_arrow = make_arrow(scr, &s_predicted_arrow_state);
  lv_obj_add_flag(s_predicted_arrow, LV_OBJ_FLAG_HIDDEN);

  // Current value as big text just above the cursor, visible in BOTH modes.
  // (Previously this lived inside the idle group at screen center; with the
  // dot strip the value's natural home is right next to the cursor — that's
  // where the user's eye is during a drag.)
  s_grind_value_label = lv_label_create(scr);
  lv_obj_set_style_text_color(s_grind_value_label, kColorText, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_grind_value_label, &lv_font_montserrat_48,
                             LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_grind_value_label, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_align(s_grind_value_label, LV_ALIGN_CENTER, 0, 110);
  lv_obj_clear_flag(s_grind_value_label, LV_OBJ_FLAG_CLICKABLE);
}

void build_idle_group(lv_obj_t* scr) {
  s_idle_group = lv_obj_create(scr);
  lv_obj_set_size(s_idle_group, kScreen, kScreen);
  lv_obj_set_pos(s_idle_group, 0, 0);
  lv_obj_set_style_bg_opa(s_idle_group, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_idle_group, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_idle_group, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_idle_group, LV_OBJ_FLAG_SCROLLABLE);
  // Group container itself is non-clickable so taps fall through to ring
  // unless they hit a child widget.
  lv_obj_clear_flag(s_idle_group, LV_OBJ_FLAG_CLICKABLE);

  // Preset row — tap to cycle.
  lv_obj_t* preset_btn = lv_button_create(s_idle_group);
  lv_obj_set_style_bg_opa(preset_btn, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(preset_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(preset_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(preset_btn, 6, LV_PART_MAIN);
  lv_obj_align(preset_btn, LV_ALIGN_CENTER, 0, -110);
  lv_obj_add_event_cb(preset_btn, on_preset_tap, LV_EVENT_CLICKED, nullptr);
  s_preset_label = lv_label_create(preset_btn);
  lv_obj_set_style_text_color(s_preset_label, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_preset_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_center(s_preset_label);

  // Climate strip — small muted line.
  s_climate_label = lv_label_create(s_idle_group);
  lv_obj_set_style_text_color(s_climate_label, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_climate_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(s_climate_label, LV_ALIGN_CENTER, 0, -76);
  lv_label_set_text(s_climate_label, "P --  H --  T --");

  // (Grind value sits above the cursor; it's created in build_ring as an
  // always-visible widget so it survives the idle→post toggle.)

  // Post button — opens the post-mode form. Sits above the always-visible
  // value text at y=+110; with the value text at +86…+134 we put the
  // button at +20 (+(-6)…+46) for clean separation.
  s_post_btn = lv_button_create(s_idle_group);
  lv_obj_set_size(s_post_btn, 140, 52);
  lv_obj_set_style_radius(s_post_btn, 26, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_post_btn, kColorAccent, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_post_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_post_btn, 0, LV_PART_MAIN);
  lv_obj_align(s_post_btn, LV_ALIGN_CENTER, 0, 20);
  lv_obj_add_event_cb(s_post_btn, on_post_tap, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* post_lbl = lv_label_create(s_post_btn);
  lv_obj_set_style_text_color(post_lbl, kColorBg, LV_PART_MAIN);
  lv_obj_set_style_text_font(post_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(post_lbl, "Post");
  lv_obj_center(post_lbl);
}

void build_post_group(lv_obj_t* scr) {
  s_post_group = lv_obj_create(scr);
  lv_obj_set_size(s_post_group, kScreen, kScreen);
  lv_obj_set_pos(s_post_group, 0, 0);
  lv_obj_set_style_bg_opa(s_post_group, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_post_group, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_post_group, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_post_group, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s_post_group, LV_OBJ_FLAG_CLICKABLE);

  // --- Time-delta stepper (top of center disk) ---
  lv_obj_t* delta_caption = lv_label_create(s_post_group);
  lv_obj_set_style_text_color(delta_caption, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(delta_caption, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(delta_caption, "time vs target");
  lv_obj_align(delta_caption, LV_ALIGN_CENTER, 0, -110);

  s_delta_label = lv_label_create(s_post_group);
  lv_obj_set_style_text_font(s_delta_label, &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_align(s_delta_label, LV_ALIGN_CENTER, 0, -68);

  const int32_t step_size = 56;
  lv_obj_t* minus = make_round_btn(s_post_group, step_size);
  lv_obj_align(minus, LV_ALIGN_CENTER, -100, -68);
  lv_obj_t* minus_lbl = lv_label_create(minus);
  lv_obj_set_style_text_color(minus_lbl, kColorText, LV_PART_MAIN);
  lv_obj_set_style_text_font(minus_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(minus_lbl, "-");
  lv_obj_center(minus_lbl);
  lv_obj_add_event_cb(minus, on_delta_minus, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* plus = make_round_btn(s_post_group, step_size);
  lv_obj_align(plus, LV_ALIGN_CENTER, 100, -68);
  lv_obj_t* plus_lbl = lv_label_create(plus);
  lv_obj_set_style_text_color(plus_lbl, kColorText, LV_PART_MAIN);
  lv_obj_set_style_text_font(plus_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(plus_lbl, "+");
  lv_obj_center(plus_lbl);
  lv_obj_add_event_cb(plus, on_delta_plus, LV_EVENT_CLICKED, nullptr);

  // --- Star row (middle) ---
  lv_obj_t* stars_caption = lv_label_create(s_post_group);
  lv_obj_set_style_text_color(stars_caption, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(stars_caption, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(stars_caption, "quality");
  lv_obj_align(stars_caption, LV_ALIGN_CENTER, 0, 0);

  const int32_t star_size = 38;
  const int32_t star_gap  = 10;
  const int32_t row_width = kMaxStars * star_size + (kMaxStars - 1) * star_gap;
  const int32_t row_x0    = kCenter - row_width / 2;
  for (uint8_t i = 0; i < kMaxStars; ++i) {
    lv_obj_t* b = make_round_btn(s_post_group, star_size);
    lv_obj_set_pos(b, row_x0 + i * (star_size + star_gap), kCenter + 20);
    lv_obj_add_event_cb(b, on_star_tap, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    s_star_btns[i] = b;
  }

  // --- Submit (bottom) ---
  s_submit_btn = lv_button_create(s_post_group);
  lv_obj_set_size(s_submit_btn, 160, 50);
  lv_obj_set_style_radius(s_submit_btn, 25, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_submit_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_submit_btn, 0, LV_PART_MAIN);
  lv_obj_align(s_submit_btn, LV_ALIGN_CENTER, 0, 90);
  s_submit_label = lv_label_create(s_submit_btn);
  lv_obj_set_style_text_font(s_submit_label, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(s_submit_label, "Submit");
  lv_obj_center(s_submit_label);
  lv_obj_add_event_cb(s_submit_btn, on_submit, LV_EVENT_CLICKED, nullptr);

  // --- Cancel — small "x" in the upper-center, gets the user back to idle
  //     without saving (in case Post was tapped by accident).
  s_cancel_btn = lv_button_create(s_post_group);
  lv_obj_set_size(s_cancel_btn, 36, 36);
  lv_obj_set_style_radius(s_cancel_btn, 18, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_cancel_btn, kColorDim, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_cancel_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_cancel_btn, 0, LV_PART_MAIN);
  lv_obj_align(s_cancel_btn, LV_ALIGN_CENTER, 0, -160);
  lv_obj_t* cancel_lbl = lv_label_create(s_cancel_btn);
  lv_obj_set_style_text_color(cancel_lbl, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(cancel_lbl, LV_SYMBOL_CLOSE);
  lv_obj_center(cancel_lbl);
  lv_obj_add_event_cb(s_cancel_btn, on_cancel_tap, LV_EVENT_CLICKED, nullptr);
}

}  // namespace

void start_report() {
  if (!display::lock()) return;

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, kColorBg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Build in z-order from bottom to top: ring overlay (rim gesture catcher) →
  // mode groups (their button widgets need to intercept taps before the ring
  // sees them). Toast on top of everything.
  build_ring(scr);
  build_idle_group(scr);
  build_post_group(scr);

  s_toast_label = lv_label_create(scr);
  lv_obj_set_style_text_color(s_toast_label, kColorAccent, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_toast_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(s_toast_label, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_add_flag(s_toast_label, LV_OBJ_FLAG_HIDDEN);

  // Seed grind value from per-preset NVS; clamp in case older firmware stored
  // something outside the new ring's range.
  s_grind_value = std::clamp(presets::last_grind(presets::selected_id()),
                             kGrindMin, kGrindMax);

  refresh_preset_label();
  refresh_grind_value_label();
  refresh_delta_label();
  refresh_stars();
  refresh_submit_enabled();
  refresh_suggestion();

  // Force a layout pass so the ring labels and arrows have concrete widths/
  // heights before refresh_ring() reads them for centering. Without this,
  // first-frame positions are off-by-half-label until LVGL gets around to
  // its own layout tick.
  lv_obj_update_layout(scr);
  refresh_ring();

  apply_mode();  // s_mode starts Idle; hides post group

  lv_timer_create(update_climate_strip, 1000, nullptr);

  display::unlock();
}

}  // namespace espressopost::ui
