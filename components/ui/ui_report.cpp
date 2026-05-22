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
constexpr int32_t kBigDotRadius    = 5;    // integer-unit dot
constexpr int32_t kSmallDotRadius  = 2;    // 0.1-step dot
constexpr float   kArcHalfDeg      = 37.5f;  // dot strip is 75° wide centered at 6 o'clock (+25%)
// Press-to-start-drag is now gated by a y-coordinate threshold rather than
// an angular slice — the user wants drags to start anywhere from the
// current-grind text down through the arc, not just on the rim itself.
constexpr int32_t kPressYThreshold = kCenter + 80;  // ~6 px above the value-text top

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
// Dial resolution lives in model::kGrindStep (model_math.hpp) so the snap
// rounding on this end and the model's "round suggestion to a dial-reachable
// value" on the other end share one source of truth.
constexpr float kDegPerUnit     = 37.5f;          // °/grind unit — feel knob (+25% from 30 for more breathing room)
constexpr float kSubDotPitchDeg = kDegPerUnit / 10.0f;  // 0.1 step = 3.75°

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
const lv_color_t kColorDark   = LV_COLOR_MAKE(0x20, 0x20, 0x20);
const lv_color_t kColorGreen  = LV_COLOR_MAKE(0x40, 0xB0, 0x60);
const lv_color_t kColorOrange = LV_COLOR_MAKE(0xD8, 0x90, 0x30);
const lv_color_t kColorRed    = LV_COLOR_MAKE(0xC8, 0x40, 0x40);

// Climate-tile icon + label accents — muted enough to stay AMOLED-friendly
// (no max-intensity sub-pixels) while reading as the named hue from a meter
// away. Labels (`PRESSURE` / `TEMPERATURE` / `HUMIDITY`) share a "bright gray"
// that sits between kColorText and kColorMuted — the icon is the colored
// signal, the label is the supporting role.
const lv_color_t kColorIconPurple = LV_COLOR_MAKE(0xA8, 0x80, 0xE0);
const lv_color_t kColorIconRed    = LV_COLOR_MAKE(0xE0, 0x70, 0x55);
const lv_color_t kColorIconBlue   = LV_COLOR_MAKE(0x60, 0xA8, 0xE0);
const lv_color_t kColorLabelGray  = LV_COLOR_MAKE(0xB0, 0xB0, 0xB0);

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
lv_obj_t* s_grind_value_label = nullptr;  // big "5.10" above the cursor — visible in idle mode
lv_obj_t* s_suggested_label   = nullptr;  // "Suggested 5.15 · 75%" below the value, above the cursor

// Idle group:
lv_obj_t* s_idle_group          = nullptr;
lv_obj_t* s_preset_btn          = nullptr;
lv_obj_t* s_preset_label        = nullptr;
lv_obj_t* s_post_btn            = nullptr;

// Climate strip — three tap-to-toggle tiles in the top 40% of the screen.
// Each tile is a button (taps cycle the unit); inside lives a custom-drawn
// icon, the section label, and a centered value+suffix block. Tiles are
// children of s_idle_group so they hide/show with the idle/post mode swap.
enum ClimateIconKind { kIconGauge, kIconThermo, kIconDrop };
struct ClimateIconState {
  ClimateIconKind kind;
  lv_color_t      color;
  // Per-kind dynamic state read by draw_climate_icon. For kIconGauge this is
  // the needle angle in degrees (0 = straight up, +90 = right, -90 = left),
  // mapped from pressure. Unused (left at 0) for kinds without a dynamic.
  float           dynamic;
};
struct ClimateTile {
  lv_obj_t*        container;    // tap-target button (whole column rect)
  lv_obj_t*        icon;         // 32×32 custom-drawn widget
  lv_obj_t*        label;        // section caption (all caps, font 14)
  lv_obj_t*        value_lbl;    // big number, font 46
  lv_obj_t*        suffix_lbl;   // inline unit suffix (°F, °C, %) — font 14
  lv_obj_t*        subtext_lbl;  // new-line suffix (inHg, hPa, "Dew Point") — font 14, centered under value
  ClimateIconState icon_state;
  int32_t          content_cx;   // local x within the container for centering
};
ClimateTile s_climate_tiles[3] = {};

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
// Flick momentum. After release, carry the user's swipe velocity for ~500 ms
// with exponential decay so the dial feels like a flywheel coasting to rest.
// Velocity is tracked in grind units / second via an EMA over PRESSING
// samples (single jittery samples shouldn't define the release velocity).
// The momentum timer feeds the same s_grind_value_raw accumulator that a
// live drag uses, so snapping / clamping / label refresh all run identically.
// ---------------------------------------------------------------------------
constexpr uint32_t kMomentumPeriodMs = 30;     // tick cadence — matches LVGL task
constexpr int      kMomentumMaxTicks = 17;     // ≈500 ms at 30 ms/tick
constexpr float    kMomentumDecay    = 0.85f;  // per tick → ~6% left after 17 ticks
constexpr float    kMomentumMinSpeed = 0.5f;   // grind units/sec — below this we stop

float       s_drag_velocity  = 0.0f;  // grind units per second, signed
uint64_t    s_drag_last_us   = 0;     // timestamp of previous PRESSING sample
lv_timer_t* s_momentum_timer = nullptr;
int         s_momentum_ticks_left = 0;

// ---------------------------------------------------------------------------
// Math helpers.
// ---------------------------------------------------------------------------
constexpr float kPi = 3.14159265f;

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
  int32_t    half_base;  // base half-width in px
  int32_t    height;     // tip-to-base distance in px
};
// Cursor is 2× the original triangle (14-px half-base, 28-px height) —
// visible from across the room without crowding the dot strip. Predicted
// indicator stays at the original size.
ArrowState s_cursor_arrow_state    = {180.0f, LV_COLOR_MAKE(0xE0, 0xE0, 0xE0), 14, 28};
ArrowState s_predicted_arrow_state = {180.0f, LV_COLOR_MAKE(0xC8, 0x40, 0x40),  7, 14};

// Per-arrow widget sizes — declared at file scope (and used by both the
// builders and the positioners) so the centering math never relies on a
// possibly-stale lv_obj_get_width. The earlier bug had the cursor offset
// 14 px right + 14 px down because lv_obj_get_width returned 0 before the
// first layout pass.
constexpr int32_t kCursorWidget    = 40;  // bounds the 28-px-tall cursor with margin
constexpr int32_t kPredictedWidget = 28;
// Push the cursor down by (cursor_h - predicted_h) / 2 = (28 - 14) / 2 = 7
// so that when the predicted arrow lands directly on the cursor (same angle),
// both tips sit at the same y. The smaller predicted arrow then nests inside
// the lower half of the cursor's silhouette; z-order (predicted created after
// the cursor in build_ring) keeps the suggested arrow visually on top.
constexpr int32_t kCursorUpOffset  = 7;

void draw_arrow_event(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  if (layer == nullptr) return;
  auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
  auto* state = static_cast<ArrowState*>(lv_obj_get_user_data(obj));
  if (state == nullptr) return;

  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  // LVGL areas are inclusive on both ends, so (x1+x2)/2 lands half a pixel
  // short of the geometric center. Use the actual width/height (x2-x1+1)
  // so the triangle's tip sits exactly on the widget's centerline.
  const float cx = coords.x1 + lv_area_get_width(&coords)  * 0.5f;
  const float cy = coords.y1 + lv_area_get_height(&coords) * 0.5f;

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

  const float hb = static_cast<float>(state->half_base);
  const float h  = static_cast<float>(state->height);
  lv_draw_triangle_dsc_t dsc;
  lv_draw_triangle_dsc_init(&dsc);
  dsc.color = state->color;
  dsc.opa   = LV_OPA_COVER;
  dsc.p[0] = rot(0.0f, +h * 0.5f);   // tip
  dsc.p[1] = rot(-hb,  -h * 0.5f);   // base-left
  dsc.p[2] = rot(+hb,  -h * 0.5f);   // base-right
  lv_draw_triangle(layer, &dsc);
}

// ---------------------------------------------------------------------------
// Dot strip — LV_EVENT_DRAW_MAIN handler. The dial value snaps to 0.1, so
// the cursor is always sitting exactly on a sub-step's dot. We emit dots
// outward from the cursor at multiples of kSubDotPitchDeg, marking the ones
// whose distance-from-cursor is a full integer in grind units as big.
//
// Dots are spaced at every 0.1 grind units — but the cursor's value snaps
// to 0.05, so half the time the cursor sits *between* two dots. We iterate
// 0.1-step indices (integers numbered in tenths of a grind unit) and
// project each one to its angular position relative to the cursor's
// current value. The cursor doesn't have to coincide with a dot.
//
// As the user drags by 0.05, every dot shifts 1.5° — half the gap to the
// next 0.1 dot — so the rim still gives smooth swipe feedback even when
// the cursor is in the in-between position.
// ---------------------------------------------------------------------------
void draw_dot_strip(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  if (layer == nullptr) return;

  // Visible range of grind values inside the arc, picked up by the dot
  // indexer below. A small over-scan keeps a dot from popping in/out at
  // the very edge of the arc.
  const float v_half = kArcHalfDeg / kDegPerUnit;
  const int32_t idx_min = static_cast<int32_t>(
      std::floor((s_grind_value - v_half) * 10.0f)) - 1;
  const int32_t idx_max = static_cast<int32_t>(
      std::ceil((s_grind_value + v_half) * 10.0f)) + 1;

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

  for (int32_t idx = idx_min; idx <= idx_max; ++idx) {
    // Sign: higher grind value than cursor = CCW of cursor in screen
    // convention (lower screen angle), so negate the (v_i − v_cur)·pitch.
    const float v_i = idx * 0.1f;
    const float angle_offset = -(v_i - s_grind_value) * kDegPerUnit;
    if (std::fabs(angle_offset) > kArcHalfDeg + 0.5f) continue;
    const float angle_deg = 180.0f + angle_offset;
    int32_t x, y;
    polar_to_screen(angle_deg, kDotRingRadius, &x, &y);
    // Big dot at every integer grind unit (= every 10 tenths). With the new
    // 30°/unit pitch the sub-dots are spaced enough that majors-every-1
    // no longer crowd them — used to be every-5 back when sub-dots merged
    // into a gray rail at the old 9°/unit density.
    const bool is_big = (idx % 10 == 0);
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
// Small "Suggested x.xx · NN%" line below the value text. Hidden when the
// model has no usable suggestion or when the post-mode form is up.
void refresh_suggested_label() {
  if (s_suggested_label == nullptr) return;
  if (s_mode != Mode::Idle || !predicted_visible(s_current_suggestion)) {
    lv_obj_add_flag(s_suggested_label, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  // U+00B7 (middle-dot) isn't in Montserrat 14's range — it rendered as a
  // missing-glyph rect. Parentheses are ASCII-clean and read just as well.
  char buf[40];
  std::snprintf(buf, sizeof(buf),
                "Suggested %.2f (%u%%)",
                static_cast<double>(s_current_suggestion.grind),
                static_cast<unsigned>(s_current_suggestion.confidence_pct));
  lv_label_set_text(s_suggested_label, buf);
  // Match the arrow's confidence tier so the label and the rim indicator
  // carry the same color signal — useful when the arrow is hidden because
  // the suggestion fell outside the arc.
  lv_obj_set_style_text_color(s_suggested_label,
                              confidence_color(s_current_suggestion.confidence_pct),
                              LV_PART_MAIN);
  lv_obj_remove_flag(s_suggested_label, LV_OBJ_FLAG_HIDDEN);
}

void refresh_predicted_arrow() {
  if (s_predicted_arrow == nullptr) return;
  if (!predicted_visible(s_current_suggestion)) {
    lv_obj_add_flag(s_predicted_arrow, LV_OBJ_FLAG_HIDDEN);
    refresh_suggested_label();
    return;
  }

  // Signed angular offset from the cursor (no wrap — the rim is no longer a
  // value scale, so wrapping makes no sense). If the suggestion falls
  // outside the visible arc, hide the arrow entirely; the suggested-value
  // text below the cursor still carries the numeric estimate (and matching
  // color tier) so the user has the info without a misleading clamped arrow.
  const float v_diff      = s_current_suggestion.grind - s_grind_value;
  const float natural_off = -v_diff * kDegPerUnit;  // + = lower angle => higher value
  if (std::fabs(natural_off) > kArcHalfDeg) {
    lv_obj_add_flag(s_predicted_arrow, LV_OBJ_FLAG_HIDDEN);
    refresh_suggested_label();
    return;
  }

  const float position_angle = 180.0f + natural_off;
  int32_t x, y;
  polar_to_screen(position_angle, kCursorRadius, &x, &y);
  // Centering math uses the known widget size — lv_obj_get_width returns 0
  // here on the very first call (before LVGL has done a layout pass), which
  // would offset the triangle by +half_widget into the lower-right.
  lv_obj_set_pos(s_predicted_arrow,
                 x - kPredictedWidget / 2,
                 y - kPredictedWidget / 2);

  // Push the new angle + color into the arrow's state and invalidate so the
  // DRAW_MAIN handler picks them up. No transforms / no fonts in play; the
  // triangle is rasterized fresh each frame from these two scalars.
  s_predicted_arrow_state.angle_deg = position_angle;
  s_predicted_arrow_state.color =
      confidence_color(s_current_suggestion.confidence_pct);
  lv_obj_invalidate(s_predicted_arrow);
  lv_obj_remove_flag(s_predicted_arrow, LV_OBJ_FLAG_HIDDEN);
  refresh_suggested_label();
}

// ---------------------------------------------------------------------------
// Center-widget refreshers.
// ---------------------------------------------------------------------------
void refresh_grind_value_label() {
  if (s_grind_value_label == nullptr) return;
  char buf[12];
  // Two-decimal readout (e.g. "5.10", "5.15") — value snaps to 0.05.
  std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(s_grind_value));
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
  // Button auto-sizes to fit the new text; re-anchor so its right edge stays
  // pinned 8 px left of POST instead of drifting onto the button.
  if (s_preset_btn != nullptr && s_post_btn != nullptr) {
    lv_obj_update_layout(s_preset_btn);
    lv_obj_align_to(s_preset_btn, s_post_btn, LV_ALIGN_OUT_LEFT_MID, -8, 0);
  }
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
  // Suggested label has its own visibility (mode + suggestion availability);
  // let it recompute.
  refresh_suggested_label();
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
  // Confidence is recorded unconditionally — even when the arrow was hidden
  // (low conf), the raw % is what lets post-hoc analysis separate "model said
  // X with 80% conf" from "X with 20% conf" rather than just "X or NaN".
  rec.confidence_pct  = s_current_suggestion.confidence_pct;
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

// Apply one frame of post-release momentum. Decays the velocity, advances
// s_grind_value_raw by velocity·dt, and re-uses the same snap + redraw path
// that the live drag uses so the visual behavior is identical. Persists the
// grind value to NVS at the end of the glide (matching the RELEASED contract).
void momentum_tick(lv_timer_t* t) {
  const float dt_s = static_cast<float>(kMomentumPeriodMs) / 1000.0f;
  s_grind_value_raw = std::clamp(s_grind_value_raw + s_drag_velocity * dt_s,
                                 kGrindMin, kGrindMax);
  const float new_value =
      std::round(s_grind_value_raw / model::kGrindStep) * model::kGrindStep;
  if (new_value != s_grind_value) {
    s_grind_value = new_value;
    refresh_ring();
    refresh_grind_value_label();
  }
  s_drag_velocity *= kMomentumDecay;
  --s_momentum_ticks_left;

  // Stop when we've coasted long enough, the velocity has decayed below the
  // noise floor, or we've pinned to the dial range. Persist on the way out
  // so the per-preset NVS slot reflects the post-glide value, not the value
  // at finger-up.
  const bool at_edge = (s_grind_value_raw <= kGrindMin + 1e-4f) ||
                       (s_grind_value_raw >= kGrindMax - 1e-4f);
  if (s_momentum_ticks_left <= 0 ||
      std::fabs(s_drag_velocity) < kMomentumMinSpeed ||
      at_edge) {
    presets::set_last_grind(presets::selected_id(), s_grind_value);
    s_drag_velocity = 0.0f;
    s_momentum_timer = nullptr;
    lv_timer_delete(t);
  }
}

void cancel_momentum() {
  if (s_momentum_timer != nullptr) {
    lv_timer_delete(s_momentum_timer);
    s_momentum_timer = nullptr;
  }
  s_drag_velocity = 0.0f;
  s_momentum_ticks_left = 0;
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

  switch (code) {
    case LV_EVENT_PRESSED: {
      // Drag starts only when the press lands at or below the current-
      // grind text — the whole lower band of the screen is now a "swipe
      // surface" so the user can scrub starting from the number, the
      // cursor, or the dot strip without needing to land exactly on the
      // rim. Once dragging, the finger can wander anywhere.
      if (p.y < kPressYThreshold) {
        s_ring_dragging = false;
        return;
      }
      // Grabbing again mid-glide stops the flywheel — the new touch should
      // own the motion, not fight a tail from the last release.
      cancel_momentum();
      s_ring_dragging   = true;
      s_drag_last_angle = std::atan2(dx, -dy) * (180.0f / kPi);
      s_drag_last_us    = esp_timer_get_time();
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
      const float new_value =
          std::round(s_grind_value_raw / model::kGrindStep) * model::kGrindStep;
      if (new_value != s_grind_value) {
        s_grind_value = new_value;
        refresh_ring();
        refresh_grind_value_label();
      }
      // Track velocity for the post-release flick. EMA with α=0.5 smooths
      // single-frame jitter (touch driver can briefly stall) while still
      // responding within ~2-3 frames to a real change in finger speed.
      const uint64_t now_us = esp_timer_get_time();
      if (s_drag_last_us != 0) {
        const float dt_s = static_cast<float>(now_us - s_drag_last_us) / 1e6f;
        if (dt_s > 1e-4f) {
          const float instant = dv / dt_s;
          s_drag_velocity = 0.5f * s_drag_velocity + 0.5f * instant;
        }
      }
      s_drag_last_us = now_us;
      break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: {
      if (!s_ring_dragging) break;
      s_ring_dragging = false;
      s_drag_last_us  = 0;
      // If the finger lifted with non-trivial speed, hand off to the
      // momentum timer — it'll persist the final value to NVS when it
      // settles. Otherwise persist immediately; there's nothing to glide.
      if (std::fabs(s_drag_velocity) >= kMomentumMinSpeed) {
        s_momentum_ticks_left = kMomentumMaxTicks;
        if (s_momentum_timer == nullptr) {
          s_momentum_timer =
              lv_timer_create(momentum_tick, kMomentumPeriodMs, nullptr);
        }
      } else {
        s_drag_velocity = 0.0f;
        presets::set_last_grind(presets::selected_id(), s_grind_value);
      }
      break;
    }
    default:
      break;
  }
}

// ---------------------------------------------------------------------------
// Climate tiles — geometry, icon drawing, value formatting, tap-to-toggle.
// ---------------------------------------------------------------------------
// Climate area occupies the top 40% of the 466-px-tall screen. The 3-column
// split is geometric thirds (466/3 ≈ 155.3); separators land at x=155 and
// x=311 in screen coords. Tile content (icon, label, value) is centered at
// these columns, with one wrinkle: the round-display chord at the icon y is
// only ~370 px wide, so the outer two columns would clip their icons if
// centered at the tile midpoint. We shift content-center inward by
// kOuterTileInset for the left+right tiles — the separator lines stay on the
// geometric thirds, but the icons, labels, and values sit visually inset
// from the round-clipped edge.
// Geometric thirds for the column splits stay put; the bottom-of-area line
// has moved down to fit the bigger glyphs (50%-up icons + 50%-up value font
// can't fit under y=186 without crowding).
constexpr int32_t kClimateBottomY  = 210;
constexpr int32_t kColLeftEdge0    = 0;
constexpr int32_t kColLeftEdge1    = 155;
constexpr int32_t kColLeftEdge2    = 311;
constexpr int32_t kColRightEdge2   = kScreen;
// Icon + label rows sit higher than the value row — feels balanced against
// the round-clip chord. Icon widgets are 60 tall (vs the 48-px visible glyph
// area) so the thermometer's cap arc and the drop's body circle both fit
// inside the widget's draw bounds — LV_EVENT_DRAW_MAIN's layer.clip_area is
// set to the widget rect, so anything past the edges gets masked.
constexpr int32_t kTileIconY       = 46;    // top y of icon widget (all three)
constexpr int32_t kTileLabelY      = 108;   // top y of section caption
// Value label uses Montserrat 46 (25% bigger than the previous 36 pt). The
// label-top y moves up ~9 px from 158 so the BASELINE of the value stays at
// the same screen y — the bigger font grows upward only, leaving the row
// below (new-line subtext) where it was.
constexpr int32_t kTileValueY      = 130;
constexpr int32_t kIconSize        = 60;
// Each separator line is pulled in by this much from both of its endpoints,
// so the verticals stop short of the icon row and the bottom horizontal
// stops short of the screen edge. Keeps the strip from looking like a
// rigid table-cell grid; the lines now read as quiet dividers, not borders.
constexpr int32_t kSeparatorInset  = 10;
// Outer-tile content shifts *outward* from the tile center so the pressure
// readout reads at the screen's left side and humidity at the right. Small
// magnitude — the icon/label pair lands ~5 px off tile center, just enough
// to differentiate the column groupings.
constexpr int32_t kOuterContentShift = -5;

// Width of separator lines
constexpr int32_t kSeparatorThickness = 2;

// Bright-gray section caption color is the only "supporting" tone in the
// climate strip — kColorLabelGray sits between kColorText and kColorMuted.

// Custom-drawn DRAW_MAIN handler — single dispatcher for all three icons.
// State lives in widget user_data (kind + color); the widget is a plain 32×32
// transparent container, the draw callback paints the outline glyph.
//
// Coordinates inside each branch are LOCAL to the icon widget (cx/cy = widget
// center); the dispatcher computes them from the widget's bounds so the icon
// doesn't need to know its absolute position.
void draw_climate_icon(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  if (layer == nullptr) return;
  auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
  auto* state = static_cast<ClimateIconState*>(lv_obj_get_user_data(obj));
  if (state == nullptr) return;

  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  // Use width/height (x2-x1+1) rather than (x1+x2)/2 so the glyph centers
  // on the geometric midpoint, not half a pixel low/right.
  const int32_t cx = coords.x1 + lv_area_get_width(&coords)  / 2;
  const int32_t cy = coords.y1 + lv_area_get_height(&coords) / 2;

  // Stroke width tracks the 50%-up icon size so the line weight scales with
  // the glyph — a 2-px stroke would look spindly at 48 px.
  constexpr int32_t kStroke = 3;

  lv_draw_line_dsc_t ld;
  lv_draw_line_dsc_init(&ld);
  ld.color = state->color;
  ld.opa   = LV_OPA_COVER;
  ld.width = kStroke;
  ld.round_start = 1;
  ld.round_end   = 1;

  lv_draw_arc_dsc_t ad;
  lv_draw_arc_dsc_init(&ad);
  ad.color = state->color;
  ad.opa   = LV_OPA_COVER;
  ad.width = kStroke;

  // Helper: outline circle via rounded-rect with bg transparent + border. Cheaper
  // than a 360° arc and avoids the start/end-cap pixel artifact.
  auto circle_outline = [&](int32_t ox, int32_t oy, int32_t r) {
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_opa       = LV_OPA_TRANSP;
    rd.border_width = kStroke;
    rd.border_color = state->color;
    rd.border_opa   = LV_OPA_COVER;
    rd.radius       = LV_RADIUS_CIRCLE;
    lv_area_t a = {ox - r, oy - r, ox + r, oy + r};
    lv_draw_rect(layer, &rd, &a);
  };

  auto line = [&](int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    ld.p1.x = x1; ld.p1.y = y1;
    ld.p2.x = x2; ld.p2.y = y2;
    lv_draw_line(layer, &ld);
  };

  // Coordinates below are tuned for a ~48-px effective drawing area (icon
  // widget is 48×48, with a few pixels of internal margin). Original 32-px
  // sizing was scaled by 1.5x and re-rounded so the geometry stays integer-
  // pixel-aligned at the new size.
  switch (state->kind) {
    case kIconGauge: {
      // Half-circle pressure gauge: arc across the top, three ticks at the
      // 9/12/3 positions, and a needle that rotates with pressure (state
      // dynamic in degrees: 0 = up, ±90 = sides). Sized 20% larger than the
      // other two glyphs — the dial reads as the visual anchor of the
      // climate row.
      ad.center.x   = cx;
      ad.center.y   = cy + 13;
      ad.radius     = 22;
      ad.start_angle = 180;
      ad.end_angle   = 360;
      lv_draw_arc(layer, &ad);
      // Ticks — short inward segments at 9, 12, and 3 o'clock (6 px long).
      line(cx - 22, cy + 13, cx - 16, cy + 13);
      line(cx,      cy -  9, cx,      cy -  3);
      line(cx + 22, cy + 13, cx + 16, cy + 13);
      // Needle: length 18 px from pivot, rotated by state->dynamic degrees
      // (clockwise from straight up). At ±90° the tip lands 4 px shy of the
      // arc rim, so the dial reads as "inside the gauge" at every reading.
      constexpr float kNeedleLen = 18.0f;
      const float ang = state->dynamic * (3.14159265f / 180.0f);
      const int32_t tx = cx + static_cast<int32_t>(kNeedleLen * std::sin(ang));
      const int32_t ty = cy + 13 - static_cast<int32_t>(kNeedleLen * std::cos(ang));
      line(cx, cy + 13, tx, ty);
      break;
    }
    case kIconThermo: {
      // Thermometer: bulb circle at bottom, stem as two parallel verticals,
      // rounded top cap as a 180° arc. Open at the stem/bulb seam — true
      // outline (no fill). Sized ~20% larger than the original 48-px glyph.
      circle_outline(cx, cy + 12, 10);
      line(cx - 5, cy - 18, cx - 5, cy + 4);
      line(cx + 5, cy - 18, cx + 5, cy + 4);
      ad.center.x   = cx;
      ad.center.y   = cy - 18;
      ad.radius     = 5;
      ad.start_angle = 180;
      ad.end_angle   = 360;
      lv_draw_arc(layer, &ad);
      break;
    }
    case kIconDrop: {
      // Water drop = outer outline shell + inner FILLED teardrop with a
      // ~2 px gap between. The shell never moves; the inner fill is
      // clipped from the top down by humidity, so the icon reads like a
      // vessel filling up — empty at 0%, brim-full at 100%.
      //
      // Outer shell: two converging tip lines + a 240° arc closing the
      // bottom. Line endpoints (tip±9, cy+5) sit at the body circle's
      // tangent points from the tip (cos⁻¹(r/d) = 60°, so tangents land
      // 30° off due-south from the body center) — drawing the arc from
      // 330° CW to 570° (=210°+360°) covers the bottom 240° and leaves
      // the top 120° open between the tip lines, so the seam reads as a
      // continuous outline.
      line(cx, cy - 12, cx - 9, cy + 4);
      line(cx, cy - 12, cx + 9, cy + 4);
      ad.center.x    = cx;
      ad.center.y    = cy + 10;
      ad.radius      = 12;
      ad.start_angle = 335;
      ad.end_angle   = 565;
      lv_draw_arc(layer, &ad);

      // Inner FILLED teardrop — filled circle (LV_RADIUS_CIRCLE on an 11×11
      // square gives r=5) + filled triangle whose base vertices sit at the
      // inner circle's tangent points from the inner tip. The tip lands at
      // (cx, cy) — the y where the outer side lines, shifted ~5.5 px
      // perpendicular inward, intersect — and the base at (cx±4, cy+7),
      // which is `(r_inner/r_outer) × outer_tangent` (= 5/11 × (cx+9.5, cy+4.5)
      // relative to the body center cy+10). Resulting inner-side slope is
      // 4/7 ≈ 0.57 vs outer 9/17 ≈ 0.53 — nearly parallel, so the gap stays
      // ~4 px around the whole perimeter rather than pinching toward the tip.
      //
      // Horizontal clip: y_level interpolates from cy at 100% down to cy+16
      // at 0%. Setting layer->_clip_area.y1 = y_level masks every draw above
      // it, so the fill drains top-down. Save/restore the prior clip so we
      // don't leak into the next tile's draws.
      const float pct = std::clamp(state->dynamic, 0.0f, 100.0f);
      const int32_t inner_top = cy - 4;
      const int32_t inner_bot = cy + 15;
      const int32_t y_level   = inner_top +
          static_cast<int32_t>(std::lround(
              (1.0f - pct / 100.0f) * (inner_bot - inner_top + 1)));

      const lv_area_t saved_clip = layer->_clip_area;
      lv_area_t       clip       = saved_clip;
      if (y_level > clip.y1) clip.y1 = y_level;
      if (clip.y1 <= clip.y2) {
        layer->_clip_area = clip;

        lv_draw_rect_dsc_t fillc;
        lv_draw_rect_dsc_init(&fillc);
        fillc.bg_opa       = LV_OPA_COVER;
        fillc.bg_color     = state->color;
        fillc.border_width = 0;
        fillc.radius       = LV_RADIUS_CIRCLE;
        lv_area_t ca = {cx - 6, cy + 5, cx + 5, cy + 15};
        lv_draw_rect(layer, &fillc, &ca);

        lv_draw_triangle_dsc_t tri;
        lv_draw_triangle_dsc_init(&tri);
        tri.color = state->color;
        tri.opa   = LV_OPA_COVER;
        tri.p[0].x = cx;     tri.p[0].y = inner_top;
        tri.p[1].x = cx - 6; tri.p[1].y = cy + 7;
        tri.p[2].x = cx + 6; tri.p[2].y = cy + 7;
        lv_draw_triangle(layer, &tri);

        layer->_clip_area = saved_clip;
      }
      break;
    }
  }
}

// Position the value + its two optional suffixes for one tile.
//
// Layout contract:
//   * VALUE is centered on the column (under the section caption), regardless
//     of either suffix's length — so "29.92" / "85" / "71.5" line up vertically
//     under "PRESSURE" / "HUMIDITY" / "TEMPERATURE" across the row.
//   * INLINE SUFFIX (°F / °C / %) tacks on to the value's right, bottoms
//     aligned. It does NOT count toward the centering budget — a long inline
//     suffix would push the row right of center rather than tugging the value
//     leftward.
//   * NEWLINE SUFFIX ("inHg" / "hPa" / "Dew Point") sits on a fresh line just
//     under the value, centered on the value's mid (which is the column
//     center). Used for long suffixes that would crowd the inline slot.
//
// Either suffix may be empty/null; the corresponding widget is hidden.
void position_value_block(ClimateTile& t, const char* val,
                          const char* inline_suf, const char* newline_suf) {
  lv_label_set_text(t.value_lbl, val);
  lv_obj_update_layout(t.value_lbl);
  const int32_t vw   = lv_obj_get_width(t.value_lbl);
  const int32_t left = t.content_cx - vw / 2;
  lv_obj_set_pos(t.value_lbl, left, kTileValueY);

  if (inline_suf == nullptr || inline_suf[0] == '\0') {
    lv_obj_add_flag(t.suffix_lbl, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text(t.suffix_lbl, inline_suf);
    lv_obj_update_layout(t.suffix_lbl);
    lv_obj_remove_flag(t.suffix_lbl, LV_OBJ_FLAG_HIDDEN);
    // Bottoms-aligned: no cap-height delta to bake in across font-size pairs.
    lv_obj_align_to(t.suffix_lbl, t.value_lbl, LV_ALIGN_OUT_RIGHT_BOTTOM, 4, 0);
  }

  if (newline_suf == nullptr || newline_suf[0] == '\0') {
    lv_obj_add_flag(t.subtext_lbl, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_label_set_text(t.subtext_lbl, newline_suf);
    lv_obj_update_layout(t.subtext_lbl);
    lv_obj_remove_flag(t.subtext_lbl, LV_OBJ_FLAG_HIDDEN);
    // Centered under the VALUE's mid (= the column center), 2 px gap below.
    lv_obj_align_to(t.subtext_lbl, t.value_lbl, LV_ALIGN_OUT_BOTTOM_MID, 0, 2);
  }
}

void refresh_climate_pressure(const climate::Reading& r) {
  ClimateTile& t = s_climate_tiles[0];
  if (t.container == nullptr) return;
  if (r.timestamp_us == 0) {
    position_value_block(t, "--", "", "");
    return;
  }
  // Pressure units ("inHg" / "hPa") are wordy — they always render as the
  // new-line subtext below the value, never inline. Keeps the big readout
  // uncluttered and reads naturally ("29.92" \n "inHg").
  char vbuf[12];
  const char* newline_suf;
  if (climate::pressure_unit() == climate::PressureUnit::InHg) {
    std::snprintf(vbuf, sizeof(vbuf), "%.2f",
                  static_cast<double>(climate::hpa_to_inhg(r.pressure_hpa)));
    newline_suf = "inHg";
  } else {
    std::snprintf(vbuf, sizeof(vbuf), "%.0f",
                  static_cast<double>(r.pressure_hpa));
    newline_suf = "hPa";
  }
  position_value_block(t, vbuf, /*inline*/"", newline_suf);

  // Drive the gauge needle from pressure: ±1 inHg around 30.00 still pegs
  // the dial at ±90°, but the curve is sqrt-shaped (|Δ|^0.5, sign preserved)
  // so small deviations move the needle visibly — e.g. ±0.1 inHg lands at
  // ~28° instead of the linear 9°. The mapping always uses inHg internally;
  // the unit toggle is presentation-only, not physical, so the dial reads
  // from the same source either way. Only invalidate when the rounded angle
  // changes a visible amount, so the icon doesn't repaint on imperceptible
  // drift.
  const float inhg     = climate::hpa_to_inhg(r.pressure_hpa);
  const float diff     = inhg - 30.0f;
  const float curved   = std::copysign(std::sqrt(std::fabs(diff)), diff);
  const float new_deg  = std::clamp(curved * 90.0f, -90.0f, 90.0f);
  if (std::fabs(new_deg - t.icon_state.dynamic) >= 1.0f) {
    t.icon_state.dynamic = new_deg;
    lv_obj_invalidate(t.icon);
  }
}

void refresh_climate_temp(const climate::Reading& r) {
  ClimateTile& t = s_climate_tiles[1];
  if (t.container == nullptr) return;
  if (r.timestamp_us == 0) {
    position_value_block(t, "--", "", "");
    return;
  }
  // °F / °C are compact symbols — they ride inline against the value.
  char vbuf[12];
  const char* inline_suf;
  if (climate::temp_unit() == climate::TempUnit::Fahrenheit) {
    std::snprintf(vbuf, sizeof(vbuf), "%.1f",
                  static_cast<double>(climate::c_to_f(r.temp_c)));
    inline_suf = "\xC2\xB0""F";
  } else {
    std::snprintf(vbuf, sizeof(vbuf), "%.1f", static_cast<double>(r.temp_c));
    inline_suf = "\xC2\xB0""C";
  }
  position_value_block(t, vbuf, inline_suf, /*newline*/"");
}

void refresh_climate_humidity(const climate::Reading& r) {
  ClimateTile& t = s_climate_tiles[2];
  if (t.container == nullptr) return;

  // The section caption stays "HUMIDITY" in both modes — the dew-point case
  // is disambiguated by a new-line "Dew Point" subtext under the value, not
  // by relabelling the column header. Keeps the row of tile captions stable
  // as the user taps through units.
  const bool dewpt = climate::humidity_unit() == climate::HumidityUnit::DewPoint;

  if (r.timestamp_us == 0) {
    position_value_block(t, "--", "", "");
    return;
  }
  char vbuf[12];
  const char* inline_suf;
  const char* newline_suf;
  if (!dewpt) {
    std::snprintf(vbuf, sizeof(vbuf), "%.0f",
                  static_cast<double>(r.humidity_pct));
    inline_suf  = "%";
    newline_suf = "";
  } else {
    // Dew point shares the temperature tile's unit choice — the user thinks
    // about temperature in one unit at a time. "Dew Point" rides below the
    // value as a new-line subtext so the reader knows the reading isn't the
    // ambient temperature.
    const float dp_c = climate::dew_point_c(r.temp_c, r.humidity_pct);
    if (climate::temp_unit() == climate::TempUnit::Fahrenheit) {
      std::snprintf(vbuf, sizeof(vbuf), "%.0f",
                    static_cast<double>(climate::c_to_f(dp_c)));
      inline_suf = "\xC2\xB0""F";
    } else {
      std::snprintf(vbuf, sizeof(vbuf), "%.0f", static_cast<double>(dp_c));
      inline_suf = "\xC2\xB0""C";
    }
    newline_suf = "Dew Point";
  }
  position_value_block(t, vbuf, inline_suf, newline_suf);

  // Drive the drop's water-level line from raw humidity %. The unit toggle
  // (% vs dew point) is presentation-only — the shell fill always reads
  // actual humidity. Only invalidate on a 1%+ change so noise doesn't
  // repaint the icon every reading.
  const float new_pct = std::clamp(r.humidity_pct, 0.0f, 100.0f);
  if (std::fabs(new_pct - t.icon_state.dynamic) >= 1.0f) {
    t.icon_state.dynamic = new_pct;
    lv_obj_invalidate(t.icon);
  }
}

void refresh_climate_tiles() {
  const climate::Reading r = climate::latest();
  refresh_climate_pressure(r);
  refresh_climate_temp(r);
  refresh_climate_humidity(r);
}

void on_pressure_tap(lv_event_t*) {
  climate::set_pressure_unit(
      climate::pressure_unit() == climate::PressureUnit::InHg
          ? climate::PressureUnit::HPa
          : climate::PressureUnit::InHg);
  refresh_climate_pressure(climate::latest());
}

void on_temp_tap(lv_event_t*) {
  climate::set_temp_unit(climate::temp_unit() == climate::TempUnit::Fahrenheit
                             ? climate::TempUnit::Celsius
                             : climate::TempUnit::Fahrenheit);
  const climate::Reading r = climate::latest();
  refresh_climate_temp(r);
  // Dew-point readout follows the temperature unit, so flipping temp also
  // updates humidity when it's in dew-point mode.
  refresh_climate_humidity(r);
}

void on_humidity_tap(lv_event_t*) {
  climate::set_humidity_unit(
      climate::humidity_unit() == climate::HumidityUnit::Percent
          ? climate::HumidityUnit::DewPoint
          : climate::HumidityUnit::Percent);
  refresh_climate_humidity(climate::latest());
}

// 1 Hz tick: refresh climate readouts + nudge the model so the arrow tracks
// ambient drift without the user touching anything.
void update_climate_strip(lv_timer_t*) {
  refresh_climate_tiles();
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

lv_obj_t* make_arrow(lv_obj_t* parent, ArrowState* state, int32_t widget_size) {
  lv_obj_t* a = lv_obj_create(parent);
  lv_obj_set_size(a, widget_size, widget_size);
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
  // Bounding box math: dots sit at radius kDotRingRadius (215). The 75° arc
  // centered at 6 o'clock spans screen-angles [142.5°, 217.5°]. The widest x
  // excursion is r·sin(37.5°) ≈ 131 from center; the highest dot is at
  // r·cos(37.5°) ≈ 170 below center, the lowest at r·cos(0°) = r (at
  // exactly 6 o'clock). Pad ±10 for the big-dot radius and a couple of
  // safety pixels.
  {
    constexpr int32_t kHalfW = 145;
    constexpr int32_t kStripH = 60;
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
  // fixed at 180° / kColorText, so no refresh needed after init. Pulled
  // up by kCursorUpOffset (= half the height difference between the two
  // arrows) so that when the predicted arrow lands at the same angle, the
  // two tips coincide and the smaller suggested arrow nests inside the
  // cursor's lower half.
  s_cursor_arrow_state.color = kColorText;
  s_static_cursor = make_arrow(scr, &s_cursor_arrow_state, kCursorWidget);
  {
    int32_t cx, cy;
    polar_to_screen(180.0f, kCursorRadius, &cx, &cy);
    lv_obj_set_pos(s_static_cursor,
                   cx - kCursorWidget / 2,
                   cy - kCursorUpOffset - kCursorWidget / 2);
  }

  // Predicted arrow — created hidden; refresh_predicted_arrow() positions
  // it on the rim at the suggested grind's angle and recolors per
  // confidence tier.
  s_predicted_arrow = make_arrow(scr, &s_predicted_arrow_state, kPredictedWidget);
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

  // "Suggested 5.15 · 75%" line — slotted between the big value text and
  // the cursor, hidden when there's no usable suggestion or in post mode.
  // 14 pt fits in the ~21 px gap between value-text bottom (~y=367) and
  // cursor top (~y=388).
  s_suggested_label = lv_label_create(scr);
  lv_obj_set_style_text_color(s_suggested_label, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_suggested_label, &lv_font_montserrat_14,
                             LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_suggested_label, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_align(s_suggested_label, LV_ALIGN_CENTER, 0, 145);
  lv_obj_clear_flag(s_suggested_label, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_suggested_label, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* make_separator(lv_obj_t* parent, int32_t x, int32_t y,
                         int32_t w, int32_t h) {
  lv_obj_t* s = lv_obj_create(parent);
  lv_obj_set_size(s, w, h);
  lv_obj_set_pos(s, x, y);
  lv_obj_set_style_bg_color(s, kColorDark, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(s, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s, LV_OBJ_FLAG_CLICKABLE);
  return s;
}

// Build one climate tile (icon + caption + value+suffix). Tile container is
// a full-column-rect button so anywhere in the column counts as a tap.
void build_climate_tile(lv_obj_t* parent, uint8_t idx,
                        int32_t left_edge, int32_t right_edge,
                        ClimateIconKind kind, lv_color_t accent,
                        const char* caption, lv_event_cb_t on_tap) {
  const int32_t tile_w = right_edge - left_edge;
  ClimateTile& t = s_climate_tiles[idx];

  t.container = lv_obj_create(parent);
  lv_obj_set_size(t.container, tile_w, kClimateBottomY);
  lv_obj_set_pos(t.container, left_edge, 0);
  lv_obj_set_style_bg_opa(t.container, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(t.container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(t.container, 0, LV_PART_MAIN);
  lv_obj_clear_flag(t.container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(t.container, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(t.container, on_tap, LV_EVENT_CLICKED, nullptr);

  // Outer tiles' content centers shift slightly outward (left for pressure,
  // right for humidity) so the columns visually weight toward their side of
  // the screen. Separators stay on the geometric thirds; only the content
  // shifts.
  if (idx == 0)      t.content_cx = tile_w / 2 - kOuterContentShift;
  else if (idx == 2) t.content_cx = tile_w / 2 + kOuterContentShift;
  else               t.content_cx = tile_w / 2;

  t.icon_state.kind  = kind;
  t.icon_state.color = accent;
  t.icon = lv_obj_create(t.container);
  lv_obj_set_size(t.icon, kIconSize, kIconSize);
  lv_obj_set_pos(t.icon, t.content_cx - kIconSize / 2, kTileIconY);
  lv_obj_set_style_bg_opa(t.icon, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(t.icon, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(t.icon, 0, LV_PART_MAIN);
  lv_obj_clear_flag(t.icon, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(t.icon, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(t.icon, &t.icon_state);
  lv_obj_add_event_cb(t.icon, draw_climate_icon, LV_EVENT_DRAW_MAIN, nullptr);

  t.label = lv_label_create(t.container);
  lv_obj_set_style_text_color(t.label, kColorLabelGray, LV_PART_MAIN);
  lv_obj_set_style_text_font(t.label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(t.label, caption);
  lv_obj_update_layout(t.label);
  lv_obj_set_pos(t.label,
                 t.content_cx - lv_obj_get_width(t.label) / 2,
                 kTileLabelY);

  // Value + the two suffix slots — populated and positioned each refresh by
  // position_value_block().
  t.value_lbl = lv_label_create(t.container);
  lv_obj_set_style_text_color(t.value_lbl, kColorText, LV_PART_MAIN);
  lv_obj_set_style_text_font(t.value_lbl, &lv_font_montserrat_46, LV_PART_MAIN);
  lv_label_set_text(t.value_lbl, "--");
  lv_obj_clear_flag(t.value_lbl, LV_OBJ_FLAG_CLICKABLE);

  t.suffix_lbl = lv_label_create(t.container);
  lv_obj_set_style_text_color(t.suffix_lbl, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(t.suffix_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(t.suffix_lbl, "");
  lv_obj_clear_flag(t.suffix_lbl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(t.suffix_lbl, LV_OBJ_FLAG_HIDDEN);

  t.subtext_lbl = lv_label_create(t.container);
  lv_obj_set_style_text_color(t.subtext_lbl, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(t.subtext_lbl, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(t.subtext_lbl, "");
  lv_obj_clear_flag(t.subtext_lbl, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(t.subtext_lbl, LV_OBJ_FLAG_HIDDEN);
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

  // --- Climate area (top 40% of the screen) ---
  // Two vertical separators on the geometric thirds, one horizontal below.
  // Lines are 2 px tall/wide. Each end of every separator is pulled in by
  // kSeparatorInset px so the strokes don't run all the way to the icon row
  // (verticals) or to the round-display chord (horizontal).
  make_separator(s_idle_group, kColLeftEdge1 - kSeparatorThickness / 2, kSeparatorInset,
                 kSeparatorThickness, kClimateBottomY - 2 * kSeparatorInset);
  make_separator(s_idle_group, kColLeftEdge2 - kSeparatorThickness / 2, kSeparatorInset,
                 kSeparatorThickness, kClimateBottomY - 2 * kSeparatorInset);
  make_separator(s_idle_group, kSeparatorInset, kClimateBottomY - kSeparatorThickness / 2,
                 kScreen - 2 * kSeparatorInset, kSeparatorThickness);

  build_climate_tile(s_idle_group, 0, kColLeftEdge0,  kColLeftEdge1,
                     kIconGauge,  kColorIconPurple, "PRESSURE",
                     on_pressure_tap);
  build_climate_tile(s_idle_group, 1, kColLeftEdge1,  kColLeftEdge2,
                     kIconThermo, kColorIconRed,    "TEMPERATURE",
                     on_temp_tap);
  build_climate_tile(s_idle_group, 2, kColLeftEdge2,  kColRightEdge2,
                     kIconDrop,   kColorIconBlue,   "HUMIDITY",
                     on_humidity_tap);

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

  // Preset selector — small tap-to-cycle label parked at the vertical middle
  // of the screen, just left of POST. The button auto-sizes to its label, so
  // refresh_preset_label re-runs the LV_ALIGN_OUT_LEFT_MID alignment after
  // every text change — otherwise the button keeps its construction-time
  // (empty-label) position and overflows rightward onto POST as it grows.
  s_preset_btn = lv_button_create(s_idle_group);
  lv_obj_set_style_bg_opa(s_preset_btn, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_preset_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_preset_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_preset_btn, 6, LV_PART_MAIN);
  lv_obj_add_event_cb(s_preset_btn, on_preset_tap, LV_EVENT_CLICKED, nullptr);
  s_preset_label = lv_label_create(s_preset_btn);
  lv_obj_set_style_text_color(s_preset_label, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_preset_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_center(s_preset_label);
  lv_obj_align_to(s_preset_btn, s_post_btn, LV_ALIGN_OUT_LEFT_MID, -8, 0);
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
  // Seed climate tiles immediately — BME280 has been sampling at 1 Hz since
  // its own task started, so a reading is usually ready by the time the UI
  // builds. Without this the strip flashes "--" for up to a second on boot.
  refresh_climate_tiles();

  apply_mode();  // s_mode starts Idle; hides post group

  lv_timer_create(update_climate_strip, 1000, nullptr);

  display::unlock();
}

}  // namespace espressopost::ui
