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
// Geometry — round AMOLED. Grinder area is a flat strip in the lower half
// (linear tick bar + upward cursor below it).
// ---------------------------------------------------------------------------
constexpr int32_t kScreen          = 466;
constexpr int32_t kCenter          = kScreen / 2;

// Horizontal divider at the top of the grinder area, drawn in idle mode only
// (hidden along with the climate strip in post mode). Per-bar PRESSED hit-test
// uses BarSpec.y_band_{top,bottom} now — this constant is purely visual.
constexpr int32_t kGrinderSeparatorY = 296;

// ---------------------------------------------------------------------------
// Grind dial — kGrindMin..kGrindMax in 0.1 steps. Linear horizontal tick bar
// with a fixed upward-pointing cursor below it; ticks scroll under the cursor
// as the value changes. kBarPxPerUnit (below) is the drag-feel knob — bigger =
// more deliberate dial, smaller = quicker.
//
// Older firmware that stored a value above kGrindMax gets clamped on UI load;
// the raw NVS float is left alone, only what we show/persist next is bounded.
// ---------------------------------------------------------------------------
constexpr float kGrindMin       =  0.0f;
constexpr float kGrindMax       = 30.0f;

// LV_ALIGN_CENTER y-offset for the big value text — sits between the
// grinder-cap separator and the bar.
constexpr int32_t kGrindValueY           = 100;

// "GRIND VALUE" caption — absolute screen coords (set_pos, not align) so it
// stays parked on the left while the centered big-number to its right grows
// and shrinks with the digit count. Y is picked to vertically center against
// the Montserrat-48 glyph row at kGrindValueY.
constexpr int32_t kGrindCaptionX         = 50;
constexpr int32_t kGrindCaptionY         = 327;

// "SUGGESTION" / "x.xx (xx%)" block mirrors GRIND VALUE on the right side of
// the big number. Both lines render center-aligned inside a fixed-width box
// so the bottom line stays horizontally locked to the caption no matter how
// the digit/percent counts shift. Block right edge sits 50 px from the screen
// edge — symmetric to the caption's 50 px left inset.
constexpr int32_t kSuggestionBlockW      = 120;
constexpr int32_t kSuggestionBlockX      = kScreen - 44 - kSuggestionBlockW;
// Shift the two-line block up so its visual center (≈ Y + 17 for two Mont14
// rows) lines up with the single-line GRIND VALUE caption center
// (≈ kGrindCaptionY + 9). That puts the caption at kGrindCaptionY - 8.
constexpr int32_t kSuggestionCaptionY    = kGrindCaptionY - 8;
constexpr int32_t kSuggestionValueY      = kSuggestionCaptionY + 18;

// Bar widget — horizontal strip centered on kBarY, total width 2·kBarHalfWidth.
// Sized to hug the round-display chord at kBarY while keeping small-tick
// spacing (kBarHalfWidth / 10 px) readable.
constexpr int32_t kBarY                  = 384;
constexpr int32_t kBarHalfWidth          = 166;
constexpr int32_t kBarStripHeight        = 36;
// Px-per-grind-unit for the suggestion-arrow position math. Derived from
// kGrindSpec.visible_half_range, so changing the bar density automatically
// keeps the suggestion arrow tracking the right tick. The bar's tick draw
// uses spec.visible_half_range directly; this constant exists because
// refresh_suggestion_arrow runs against grind specifically.
// (Defined inline below kGrindSpec — kGrindSpec isn't visible yet here.)

// Tick sizes. Big = integer (1.0 step), mid = 0.5 step, small = 0.1 step.
// Big ticks are taller AND thicker so integer landmarks read pre-attentively;
// mid ticks are "long hairlines" that mark half-unit boundaries without
// competing with the integers; small ticks are short hairlines for fine feel.
constexpr int32_t kBigTickLen         = 26;
constexpr int32_t kBigTickThickness   = 3;
constexpr int32_t kMidTickLen         = 18;
constexpr int32_t kMidTickThickness   = 1;
constexpr int32_t kSmallTickLen       = 8;
constexpr int32_t kSmallTickThickness = 1;
// Background rail height — matches the small (0.1) tick so the rail reads as
// the substrate those ticks sit on; big and mid ticks extend past it.
constexpr int32_t kBarStripBgHeight   = 14;
// Tick colors are declared after the kColor* palette below.

// Time-delta bar — value is the user's reported actual brew time in seconds.
// Range starts at 0 (a coffee can't run negative time) and tops out below the
// 3-digit threshold (a 100+ s pull means the basket is choked and the user is
// dumping it, not journaling it). The stored ShotRecord field is signed delta
// from the preset's target, computed at submit time.
constexpr float kDeltaMin = 0.0f;
constexpr float kDeltaMax = 99.0f;
constexpr float kDeltaStep = 1.0f;

constexpr uint8_t kMaxStars = 5;

// ---------------------------------------------------------------------------
// AMOLED-friendly muted palette — pure-black background, no max-intensity
// sub-pixels (saves burn-in). Same logic as the bringup screen.
//
// COLOR(0xRRGGBB) is a thin shim over LV_COLOR_MAKE so palette entries read
// like a CSS hex literal instead of three comma-separated bytes — easier to
// paste a Figma swatch in unchanged, and easier to eyeball red/green/blue
// channels at a glance.
// ---------------------------------------------------------------------------
#define COLOR(rgb) LV_COLOR_MAKE(((rgb) >> 16) & 0xFF, \
                                 ((rgb) >> 8)  & 0xFF, \
                                 (rgb)         & 0xFF)

const lv_color_t kColorBg     = COLOR(0x000000);
const lv_color_t kColorAccent = COLOR(0xC88036);
const lv_color_t kColorText   = COLOR(0xE0E0E0);
const lv_color_t kColorMuted  = COLOR(0x707070);
const lv_color_t kColorMuted2 = COLOR(0x505050);
const lv_color_t kColorMuted3 = COLOR(0x303030);
const lv_color_t kColorMuted4 = COLOR(0x202020);

// Confidence-tier colors for the suggestion arrow + SUGGESTION block. Named
// for the tier they represent rather than the hue so callers (confidence_color
// and anything else that surfaces the tier) don't have to know that "low"
// happens to be red today.
const lv_color_t kColorConfidenceLow  = COLOR(0xE07055);
const lv_color_t kColorConfidenceMed  = COLOR(0xA880E0);
const lv_color_t kColorConfidenceHigh = COLOR(0x60A8E0);

// Climate-tile icon + label accents — muted enough to stay AMOLED-friendly
// (no max-intensity sub-pixels) while reading as the named hue from a meter
// away. Labels (`PRESSURE` / `TEMPERATURE` / `HUMIDITY`) share a "bright gray"
// that sits between kColorText and kColorMuted — the icon is the colored
// signal, the label is the supporting role.
const lv_color_t kColorIconPressure = COLOR(0xA880E0);
const lv_color_t kColorIconTemp     = COLOR(0xE07055);
const lv_color_t kColorIconHumidity = COLOR(0x60A8E0);
const lv_color_t kColorLabel        = COLOR(0xB0B0B0);

// Grinder tick colors. Mid + small share a tier so half-unit ticks read as
// "longer hairlines" rather than a third distinct stratum.
const lv_color_t kBigTickColor   = kColorMuted;
const lv_color_t kMidTickColor   = kColorMuted;
const lv_color_t kSmallTickColor = kColorMuted2;
const lv_color_t kBarStripBgColor = COLOR(0x28232F);

// ---------------------------------------------------------------------------
// UI modes. The grinder bar + cursor + value text stay live across both;
// only the center widgets swap.
// ---------------------------------------------------------------------------
enum class Mode { Idle, Post };
Mode s_mode = Mode::Idle;

// ---------------------------------------------------------------------------
// Bar generalization. Grind and Time-Delta share the same scroll-with-momentum
// chassis (draw, drag, flick, snap). BarSpec is the value-domain config (range,
// snap step, tick tiers, screen y); BarState is per-instance runtime (current
// value, drag/momentum bookkeeping, sticky-touched flag, the lv_obj widget
// hosting the custom draw, optional hooks for change/settle/touched).
// ---------------------------------------------------------------------------
struct BarSpec {
  float    min;
  float    max;
  float    step;                  // snap grid; value rounds here at settle
  float    visible_half_range;    // value units shown from cursor center to bar edge
  int32_t  y;                     // screen y of bar centerline
  // PRESSED hit-test band in screen y. Wider than the visible bar so a slightly
  // mistargeted swipe still lands. The two bars' bands must NOT overlap, or a
  // single press would race both bars' drag flags.
  int32_t  y_band_top;
  int32_t  y_band_bottom;
  int      big_every;             // 1-of-N tick indices is "big"   (integer in grind; 10 s in delta)
  int      mid_every;             // 1-of-N tick indices is "mid"   (half-unit grind; 5 s delta)
  float    tick_unit;             // value step between adjacent ticks (0.1 grind, 1 s delta)
};

struct BarState;
using BarHook = void (*)(BarState*);

struct BarState {
  const BarSpec* spec;
  lv_obj_t* widget;        // custom-drawn lv_obj; tick paint goes here
  float     value;         // free-floating during drag/glide; snapped to spec.step on settle
  bool      touched;       // sticky — was this bar swiped since the last form reset?

  // Drag bookkeeping. `dragging` is set in PRESSED only when the press lands
  // inside spec.y_band_{top,bottom}, cleared in RELEASED / PRESS_LOST.
  // PRESSING is a no-op until `dragging` is true.
  bool      dragging;
  int32_t   last_x;
  uint64_t  last_us;

  // Flick momentum — same kMomentum* cadence/decay shared across both bars.
  float       velocity;            // value units / sec, signed
  lv_timer_t* momentum_timer;
  int         momentum_ticks_left;

  // Hooks. on_change fires whenever value moves (drag step or momentum step or
  // settle-snap). on_settle fires once when drag/glide ends, after snap.
  // on_touched fires once per form session when `touched` flips false→true —
  // the Post form uses it to enable Submit. All nullable.
  BarHook on_change;
  BarHook on_settle;
  BarHook on_touched;
};

// Momentum cadence + decay are global; per-bar state lives in BarState.
constexpr uint32_t kMomentumPeriodMs = 30;     // tick cadence
constexpr int      kMomentumMaxTicks = 17;     // ≈500 ms at 30 ms/tick
constexpr float    kMomentumDecay    = 0.85f;  // per tick → ~6% left after 17 ticks
constexpr float    kMomentumMinSpeed = 0.5f;   // value units/sec — below this we stop

// Time-delta bar lives in the post group only — mirror image of the grind
// area: cursor at the TOP of the screen pointing DOWN at the bar, bar
// directly below, value readout (BREW TIME caption + big number + DELTA
// block) below that. Big tick lands on every 10 s, mid tick on every 5 s.
constexpr int32_t kDeltaBarY = 82;

// y-band bounds.
//   - Grind keeps the legacy "everything below the cap separator" range —
//     the bottom area (bar + cursor + big number + captions + SUGGESTION) is
//     identical in idle and post, so the band shouldn't change either.
//   - Delta mirrors grind: it owns everything from the top edge of the
//     screen down to just above the BREW TIME readout, so a press anywhere
//     above the readout (including on the downward cursor) starts a scrub.
//   - The bands cannot overlap. Quiet zone here is y=101–297.
// visible_half_range × 2 is the total value span across the bar's width.
// Smaller half-range → more px per tick → finer drag-feel without touching
// kBarHalfWidth. Tune here when the dial feels too twitchy or too coarse.
constexpr BarSpec kGrindSpec = {
  /*min*/                kGrindMin,
  /*max*/                kGrindMax,
  /*step*/               model::kGrindStep,
  /*visible_half_range*/ 0.75f,
  /*y*/                  kBarY,
  /*y_band_top*/         298,
  /*y_band_bottom*/      kScreen,
  /*big_every*/          10,
  /*mid_every*/          5,
  /*tick_unit*/          0.1f,
};

// Pixels per grind unit, derived from kGrindSpec so changing the bar density
// automatically keeps refresh_suggestion_arrow's position math correct.
constexpr float kBarPxPerUnit =
    static_cast<float>(kBarHalfWidth) / kGrindSpec.visible_half_range;

constexpr BarSpec kDeltaSpec = {
  /*min*/                kDeltaMin,
  /*max*/                kDeltaMax,
  /*step*/               kDeltaStep,
  /*visible_half_range*/ 7.5f,
  /*y*/                  kDeltaBarY,
  /*y_band_top*/         0,
  /*y_band_bottom*/      100,
  /*big_every*/          10,
  /*mid_every*/          5,
  /*tick_unit*/          1.0f,
};

// ---------------------------------------------------------------------------
// Widget handles. Grouped by visual role; null until start_report() builds them.
// ---------------------------------------------------------------------------
// Grinder-area extras (always visible across Idle + Post):
lv_obj_t* s_grinder_overlay   = nullptr;  // transparent full-screen swipe catcher; dispatches to both bars
lv_obj_t* s_static_cursor     = nullptr;  // upward-pointing triangle just below the grind bar
lv_obj_t* s_suggestion_arrow  = nullptr;  // confidence-tinted suggestion triangle over the cursor
lv_obj_t* s_grind_value_label = nullptr;  // big "5.10" above the bar — visible in idle mode
lv_obj_t* s_grind_caption      = nullptr; // "GRIND VALUE" header next to the big number
lv_obj_t* s_suggestion_caption = nullptr; // static "SUGGESTION" header, right of the big value
lv_obj_t* s_suggested_label   = nullptr;  // "x.xx (xx%)" value line under SUGGESTION caption

// The two bars themselves. spec is wired at start_report(); widgets at
// build_grinder. Grind is shown in both modes; delta only in post.
BarState s_grind = {};
BarState s_delta = {};

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
  // 0..1 boot-intro factor. The draw routine lerps each icon's visualization
  // from its "empty" pose (needle parked at −90°, mercury at 0 %, drop empty)
  // toward `dynamic` using this progress, so the strip animates from blank to
  // its first reading on power-on. Stays at 1 once the intro anim finishes;
  // subsequent updates just write `dynamic` and redraw at full progress.
  float           intro_progress;
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
lv_obj_t* s_delta_cursor        = nullptr;  // downward-pointing arrow at the top, mirror of the grind cursor
lv_obj_t* s_brew_time_caption   = nullptr;  // "BREW TIME" caption left of the value, mirror of GRIND VALUE
lv_obj_t* s_brew_time_value     = nullptr;  // big "30s" readout, centered (Mont 36)
lv_obj_t* s_brew_delta_caption  = nullptr;  // "DELTA" header right of value, mirror of SUGGESTION caption
lv_obj_t* s_brew_delta_value    = nullptr;  // "+/-Ns" tinted by abs(delta), mirror of suggestion value line
lv_obj_t* s_preset_post_label   = nullptr;  // read-only preset string parked on the center line in post mode
lv_obj_t* s_quality_caption     = nullptr;  // "QUALITY" caption to the left of the star row
lv_obj_t* s_star_btns [kMaxStars] = {};      // transparent tap targets sized to each star
lv_obj_t* s_star_icons[kMaxStars] = {};      // custom-drawn star widgets (outline when unlit, filled fan when lit)
// Per-star lit/unlit flag read by draw_star_event each paint. Declared as a
// forward-friendly POD struct so refresh_stars (which lives earlier in the
// file) can flip flags without needing the full draw helper visible yet.
struct StarState { bool lit; };
StarState s_star_states[kMaxStars] = {};
lv_obj_t* s_sour_btn       = nullptr;
lv_obj_t* s_sour_label     = nullptr;
lv_obj_t* s_bitter_btn     = nullptr;
lv_obj_t* s_bitter_label   = nullptr;
lv_obj_t* s_submit_btn     = nullptr;
lv_obj_t* s_submit_label   = nullptr;
lv_obj_t* s_cancel_btn     = nullptr;

// Toast (transient):
lv_obj_t* s_toast_label         = nullptr;
lv_timer_t* s_toast_timer       = nullptr;

// ---------------------------------------------------------------------------
// Form / model state.
// ---------------------------------------------------------------------------
model::Suggestion s_current_suggestion = {std::nanf(""), 0};

uint8_t s_stars_value = 0;
// Bitfield of kTasteSour | kTasteBitter — toggled by the two Post pills. Reset
// on enter_idle. Submitted into ShotRecord.taste_flags verbatim.
uint8_t s_taste_flags = 0;

// ---------------------------------------------------------------------------
// Math helpers.
// ---------------------------------------------------------------------------
constexpr float kPi = 3.14159265f;

// Confidence-tier color for the suggestion dot. Caller is responsible for
// gating the marker behind confidence_pct > 30 (this returns red for that
// band just for completeness; nothing should ever render it).
lv_color_t confidence_color(uint8_t pct) {
  if (pct > 80) return kColorConfidenceHigh;
  if (pct > 50) return kColorConfidenceMed;
  return kColorConfidenceLow;
}

bool predicted_visible(const model::Suggestion& s) {
  return s.confidence_pct > 30 &&
         !std::isnan(s.grind) &&
         s.grind >= kGrindMin &&
         s.grind <= kGrindMax;
}

// ---------------------------------------------------------------------------
// Cursor triangle. Original implementation used a label with U+25BC "▼", but
// Montserrat doesn't ship the geometric-shapes range so the glyph fell back
// to a missing-glyph rectangle. We draw the triangle ourselves via
// lv_draw_triangle, with the angle + color stored in the widget's
// user_data so the DRAW_MAIN handler picks them up per frame.
// ---------------------------------------------------------------------------
struct ArrowState {
  float      angle_deg;  // 0° = tip pointing UP (toward the bar above)
  lv_color_t color;
  int32_t    half_base;  // base half-width in px
  int32_t    height;     // tip-to-base distance in px
};
// Cursor and suggestion arrows share a widget bound (kCursorWidget, wider
// than either triangle to give the rotated rasterizer antialias headroom)
// and a tip-y on screen; the suggestion arrow is 2 px shorter so the cursor
// reads as the anchor underneath when the two overlap. kCursorWidget is
// used by the centering math directly (lv_obj_get_width returns 0 before
// LVGL's first layout pass). kCursorTipGap is the gap between the bar's
// big-tick bottom edge and the triangle tip.
constexpr int32_t kCursorWidget            = 20;
constexpr int32_t kCursorArrowHalfBase     = 10;
constexpr int32_t kCursorArrowHeight       = 24;
constexpr int32_t kCursorTipGap            = 15;
constexpr int32_t kSuggestionArrowHalfBase = 6;
constexpr int32_t kSuggestionArrowHeight   = 14;
constexpr int32_t kSuggestionArrowTipGap   = -4;

ArrowState s_cursor_arrow_state = {0.0f, COLOR(0xE0E0E0),
                                   kCursorArrowHalfBase, kCursorArrowHeight};
// Mirror of the grind cursor — 180° so the tip points DOWN at the delta bar
// from above. Same size as the grind cursor for visual parity.
ArrowState s_delta_cursor_state  = {180.0f, COLOR(0xE0E0E0),
                                    kCursorArrowHalfBase, kCursorArrowHeight};
// Suggestion arrow color is reassigned per confidence tier on every refresh;
// initial value is just a placeholder until the first refresh_grinder().
ArrowState s_suggestion_arrow_state = {0.0f, COLOR(0xE0E0E0),
                                       kSuggestionArrowHalfBase,
                                       kSuggestionArrowHeight};

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
  // To make the tip point in direction θ (0° = up, 90° = right, 180° = down),
  // rotate CW by (θ − 180°). Rotation matrix in screen y-down coords matches
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
// Grinder bar — LV_EVENT_DRAW_MAIN handler. The dial value floats freely while
// dragging (snapping only at release / momentum-end), so the cursor (fixed at
// bar center) typically sits *between* the 0.1-step ticks. We iterate
// 0.1-step indices (integers numbered in tenths of a grind unit) and project
// each one to its x-position on the bar relative to the cursor's current
// value. The cursor doesn't have to coincide with a tick.
//
// Direction convention: standard horizontal scale. LOWER (finer) values
// sit LEFT of the cursor; HIGHER (coarser) values sit RIGHT. Drag is
// direct manipulation — the bar follows the finger. Swipe right → ticks
// scroll RIGHT under the fixed cursor → cursor reads a LOWER value
// (lower numbers were just off-screen to the LEFT, now rolled in).
// ---------------------------------------------------------------------------
// Generic bar tick painter. Pulls BarState* off the widget's user_data, reads
// the spec's range / tick rules, and emits the bg rail + tick rects centered
// on the widget's coord box. Out-of-range ticks (below spec.min or above
// spec.max) are clipped — the delta bar can't go under 0 s, so ticks below
// 0 must not render even when the cursor sits near the floor.
void draw_bar_event(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  if (layer == nullptr) return;
  auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
  // BarState lives on the WIDGET (lv_obj_set_user_data in make_bar_widget),
  // not on the callback registration — same pattern as draw_arrow_event /
  // draw_climate_icon. lv_event_get_user_data would return the per-callback
  // pointer (nullptr in our registration).
  auto* state = static_cast<BarState*>(lv_obj_get_user_data(obj));
  if (state == nullptr || state->spec == nullptr) return;
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  const int32_t cx = coords.x1 + lv_area_get_width(&coords)  / 2;
  const int32_t cy = coords.y1 + lv_area_get_height(&coords) / 2;

  const BarSpec& spec = *state->spec;
  const float    value = state->value;
  const float    px_per_unit =
      static_cast<float>(kBarHalfWidth) / spec.visible_half_range;
  const float    inv_tick = 1.0f / spec.tick_unit;

  // Background rail. Drawn first so the ticks paint on top; sized to the
  // small-tick height so the big and mid ticks still stick proud of the rail.
  // Pill ends — LV_RADIUS_CIRCLE rounds both caps to half-height without
  // touching the straight middle section.
  {
    lv_draw_rect_dsc_t bg_dsc;
    lv_draw_rect_dsc_init(&bg_dsc);
    bg_dsc.bg_color = kBarStripBgColor;
    bg_dsc.bg_opa   = LV_OPA_COVER;
    bg_dsc.radius   = LV_RADIUS_CIRCLE;
    lv_area_t bg_a = {cx - kBarHalfWidth, cy - kBarStripBgHeight / 2,
                      cx + kBarHalfWidth, cy + kBarStripBgHeight / 2};
    lv_draw_rect(layer, &bg_dsc, &bg_a);
  }

  // Visible range of values across the bar, plus one tick of over-scan so a
  // tick doesn't pop in/out at the very edge as the value scrolls.
  const int32_t idx_min = static_cast<int32_t>(
      std::floor((value - spec.visible_half_range) * inv_tick)) - 1;
  const int32_t idx_max = static_cast<int32_t>(
      std::ceil((value + spec.visible_half_range) * inv_tick)) + 1;

  lv_draw_rect_dsc_t big_dsc;
  lv_draw_rect_dsc_init(&big_dsc);
  big_dsc.bg_color = kBigTickColor;
  big_dsc.bg_opa   = LV_OPA_COVER;

  lv_draw_rect_dsc_t mid_dsc;
  lv_draw_rect_dsc_init(&mid_dsc);
  mid_dsc.bg_color = kMidTickColor;
  mid_dsc.bg_opa   = LV_OPA_COVER;

  lv_draw_rect_dsc_t small_dsc;
  lv_draw_rect_dsc_init(&small_dsc);
  small_dsc.bg_color = kSmallTickColor;
  small_dsc.bg_opa   = LV_OPA_COVER;

  // Half a tick of slack on the value-domain clip so a tick that sits exactly
  // at spec.min / spec.max still draws (floating-point rounding shouldn't
  // erase the floor tick).
  const float clip_slack = spec.tick_unit * 0.5f;

  for (int32_t idx = idx_min; idx <= idx_max; ++idx) {
    const float v_i = idx * spec.tick_unit;
    if (v_i < spec.min - clip_slack) continue;
    if (v_i > spec.max + clip_slack) continue;
    // Standard slider direction: HIGHER value sits RIGHT of cursor.
    const float x_offset = (v_i - value) * px_per_unit;
    if (std::fabs(x_offset) > static_cast<float>(kBarHalfWidth) + 0.5f) continue;
    const int32_t tick_x = cx + static_cast<int32_t>(std::lround(x_offset));
    // Tier: big_every (e.g. integers for grind, 10 s for delta) > mid_every
    // (half-unit grind, 5 s delta) > everything else. Mid + small share
    // thickness; only length and dsc differ.
    const bool is_big = (idx % spec.big_every == 0);
    const bool is_mid = !is_big && (idx % spec.mid_every == 0);
    int32_t half_h, half_w;
    lv_draw_rect_dsc_t* dsc;
    if (is_big) {
      half_h = kBigTickLen / 2;
      half_w = kBigTickThickness / 2;
      dsc    = &big_dsc;
    } else if (is_mid) {
      half_h = kMidTickLen / 2;
      half_w = kMidTickThickness / 2;
      dsc    = &mid_dsc;
    } else {
      half_h = kSmallTickLen / 2;
      half_w = kSmallTickThickness / 2;
      dsc    = &small_dsc;
    }
    lv_area_t a = {tick_x - half_w, cy - half_h,
                   tick_x + half_w, cy + half_h};
    lv_draw_rect(layer, dsc, &a);
  }
}

// ---------------------------------------------------------------------------
// Grinder refresh — invalidate the bar widget (LVGL only repaints its bounds,
// not the screen) and reposition the suggestion arrow over the cursor at the
// model's grind value.
// ---------------------------------------------------------------------------

// Reposition + recolor the suggestion arrow. Hidden when the model has no
// usable suggestion or when its x scrolls outside the visible bar range
// (the "Suggested x.xx (NN%)" text below still carries the number + matching
// color tier in that case).
void refresh_suggestion_arrow() {
  if (s_suggestion_arrow == nullptr) return;
  if (!predicted_visible(s_current_suggestion)) {
    lv_obj_add_flag(s_suggestion_arrow, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  const float offset_px = (s_current_suggestion.grind - s_grind.value) * kBarPxPerUnit;
  if (std::fabs(offset_px) > static_cast<float>(kBarHalfWidth)) {
    lv_obj_add_flag(s_suggestion_arrow, LV_OBJ_FLAG_HIDDEN);
    return;
  }
  s_suggestion_arrow_state.color = confidence_color(s_current_suggestion.confidence_pct);
  const int32_t tip_y    = kBarY + kBigTickLen / 2 + kSuggestionArrowTipGap;
  const int32_t tip_inset = (kCursorWidget - kSuggestionArrowHeight) / 2;
  const int32_t tip_x    = kCenter + static_cast<int32_t>(std::lround(offset_px));
  lv_obj_set_pos(s_suggestion_arrow,
                 tip_x - kCursorWidget / 2,
                 tip_y - tip_inset);
  lv_obj_remove_flag(s_suggestion_arrow, LV_OBJ_FLAG_HIDDEN);
  // Force a repaint so a color-only change (same position, new confidence
  // tier) still renders — lv_obj_set_pos to an unchanged y/x is a no-op.
  lv_obj_invalidate(s_suggestion_arrow);
}

// Right-side SUGGESTION block. Caption + value are toggled as a pair so the
// header never floats above empty space. The bottom area is shared across
// idle and post, so this block stays visible in both modes — only model
// availability gates it now:
//   - No usable model output → muted "N/A" placeholder so the spot doesn't
//     appear/disappear as data trickles in; reads as "this slot exists,
//     just nothing to say yet".
//   - Suggestion available → live "x.xx (NN%)" tinted by the confidence
//     tier; caption shares the tier color so the whole block carries it.
void refresh_suggested_label() {
  if (s_suggested_label == nullptr) return;
  if (!predicted_visible(s_current_suggestion)) {
    lv_label_set_text(s_suggested_label, "N/A");
    lv_obj_set_style_text_color(s_suggested_label, kColorMuted3, LV_PART_MAIN);
    lv_obj_remove_flag(s_suggested_label, LV_OBJ_FLAG_HIDDEN);
    if (s_suggestion_caption != nullptr) {
      lv_obj_set_style_text_color(s_suggestion_caption, kColorMuted3,
                                  LV_PART_MAIN);
      lv_obj_remove_flag(s_suggestion_caption, LV_OBJ_FLAG_HIDDEN);
    }
    return;
  }
  char buf[24];
  std::snprintf(buf, sizeof(buf),
                "%.2f (%u%%)",
                static_cast<double>(s_current_suggestion.grind),
                static_cast<unsigned>(s_current_suggestion.confidence_pct));
  lv_label_set_text(s_suggested_label, buf);
  const lv_color_t tier = confidence_color(s_current_suggestion.confidence_pct);
  lv_obj_set_style_text_color(s_suggested_label, tier, LV_PART_MAIN);
  lv_obj_remove_flag(s_suggested_label, LV_OBJ_FLAG_HIDDEN);
  if (s_suggestion_caption != nullptr) {
    lv_obj_set_style_text_color(s_suggestion_caption, tier, LV_PART_MAIN);
    lv_obj_remove_flag(s_suggestion_caption, LV_OBJ_FLAG_HIDDEN);
  }
}

void refresh_grinder() {
  if (s_grind.widget != nullptr) {
    lv_obj_invalidate(s_grind.widget);
  }
  refresh_suggestion_arrow();
  refresh_suggested_label();
}

// ---------------------------------------------------------------------------
// Center-widget refreshers.
// ---------------------------------------------------------------------------
void refresh_grind_value_label() {
  if (s_grind_value_label == nullptr) return;
  char buf[12];
  // Two-decimal readout (e.g. "5.10", "5.15"). The bar position floats
  // sub-step during a drag/glide, so round to the 0.05 grid here — otherwise
  // the number would twitch across ten sub-step values for one finger nudge.
  const float displayed =
      std::round(s_grind.value / model::kGrindStep) * model::kGrindStep;
  std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(displayed));
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

// Read-only preset string parked on the post-mode center line. Same format as
// refresh_preset_label's idle button, but the post variant is plain text — no
// cycling, since changing presets mid-form would invalidate the delta default.
void refresh_preset_post_label() {
  if (s_preset_post_label == nullptr) return;
  const auto p = presets::get(presets::selected_id());
  char buf[40];
  std::snprintf(buf, sizeof(buf), "%s  \xC2\xB7  target %us", p.name,
                static_cast<unsigned>(p.target_time_s));
  lv_label_set_text(s_preset_post_label, buf);
}

// Tier color for the DELTA readout — same palette as confidence_color but
// keyed on |delta|. Tight pulls (|Δt| < 3 s) read as blue, mid drift as
// purple, wide misses as red. Mirrors the confidence-tier visual cue so the
// user reads color the same way across both Post readouts.
lv_color_t delta_color(int delta_s) {
  const int abs_d = std::abs(delta_s);
  if (abs_d < 3) return kColorConfidenceHigh;
  if (abs_d < 6) return kColorConfidenceMed;
  return kColorConfidenceLow;
}

// BREW TIME row — value (e.g. "30s") + DELTA block ("+/-Ns" tinted by tier).
// Called from delta_on_change so the readout tracks the bar value frame by
// frame, and from reset_post_form / enter_post on seed.
void refresh_brew_time_labels() {
  const int brew_s   = static_cast<int>(std::lround(s_delta.value));
  const auto p       = presets::get(presets::selected_id());
  const int delta_s  = brew_s - static_cast<int>(p.target_time_s);

  if (s_brew_time_value != nullptr) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%ds", brew_s);
    lv_label_set_text(s_brew_time_value, buf);
  }
  if (s_brew_delta_value != nullptr) {
    char buf[8];
    // Show a sign on non-zero deltas ("+3s" / "-3s"); plain "0s" reads cleaner
    // than "+0s" for a pull that landed exactly on target.
    if (delta_s == 0) std::snprintf(buf, sizeof(buf), "0s");
    else              std::snprintf(buf, sizeof(buf), "%+ds", delta_s);
    lv_label_set_text(s_brew_delta_value, buf);
    // Caption and value share the tier color so the whole block reads as
    // one unit — same trick the SUGGESTION block uses with confidence_color.
    const lv_color_t tier = delta_color(delta_s);
    lv_obj_set_style_text_color(s_brew_delta_value, tier, LV_PART_MAIN);
    if (s_brew_delta_caption != nullptr) {
      lv_obj_set_style_text_color(s_brew_delta_caption, tier, LV_PART_MAIN);
    }
  }
}

// Flip each star's lit flag against s_stars_value and invalidate so the
// custom draw handler repaints. Lit → filled accent fan; unlit → muted
// outline. Tap targets stay transparent; only the star icon repaints.
void refresh_stars() {
  for (uint8_t i = 0; i < kMaxStars; ++i) {
    s_star_states[i].lit = (i < s_stars_value);
    if (s_star_icons[i] != nullptr) {
      lv_obj_invalidate(s_star_icons[i]);
    }
  }
}

// Recolor the Sour / Bitter pills against s_taste_flags. Off = muted bg +
// muted text; on = accent bg + bg-color text (so the label reads against the
// accent fill).
void refresh_taste_toggles() {
  struct { lv_obj_t* btn; lv_obj_t* lbl; uint8_t mask; } pills[] = {
      {s_sour_btn,   s_sour_label,   storage::kTasteSour},
      {s_bitter_btn, s_bitter_label, storage::kTasteBitter},
  };
  for (auto& pill : pills) {
    if (pill.btn == nullptr) continue;
    const bool on = (s_taste_flags & pill.mask) != 0;
    lv_obj_set_style_bg_color(pill.btn,
                              on ? kColorAccent : kColorMuted3, LV_PART_MAIN);
    if (pill.lbl != nullptr) {
      lv_obj_set_style_text_color(pill.lbl,
                                  on ? kColorBg : kColorMuted, LV_PART_MAIN);
    }
  }
}

void refresh_submit_enabled() {
  if (s_submit_btn == nullptr) return;
  // Submit unlocks only after BOTH quality is set AND the delta bar has been
  // touched. The touched gate prevents a one-tap submit that records the
  // preset's default brew time verbatim — even a "delta zero" pull should be
  // an explicit confirmation, not the form's initial state.
  const bool ready = s_stars_value > 0 && s_delta.touched;
  if (ready) {
    lv_obj_remove_state(s_submit_btn, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(s_submit_btn, kColorAccent, LV_PART_MAIN);
    lv_obj_set_style_text_color(s_submit_label, kColorBg, LV_PART_MAIN);
  } else {
    lv_obj_add_state(s_submit_btn, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(s_submit_btn, kColorMuted3, LV_PART_MAIN);
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
  refresh_grinder();
}

// ---------------------------------------------------------------------------
// Mode transitions. s_idle_group holds the climate strip + idle center-line
// widgets (POST btn, preset btn). s_post_group holds the post-form UI +
// post-mode center-line widgets (✕, preset readonly, Submit). The bottom
// area below the climate-bottom separator at y=210 — center-line position
// included — is identical in both modes: grind bar + cursor + suggestion
// arrow + big number + GRIND VALUE caption + SUGGESTION block. apply_mode
// just swaps the two mode groups; nothing else changes between modes.
// ---------------------------------------------------------------------------
void apply_mode() {
  if (s_mode == Mode::Idle) {
    lv_obj_remove_flag(s_idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_post_group, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_idle_group, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_post_group, LV_OBJ_FLAG_HIDDEN);
  }
  // SUGGESTION block is shown in both modes now; let refresh_suggested_label
  // recompute its visibility based on suggestion availability alone.
  refresh_suggested_label();
}

// Reset post-form state to the preset's defaults. Pulled out of enter_idle so
// enter_post can share the same reseed (e.g. preset target time → delta bar
// seed) without duplicating the logic. Grind survives — the user already
// dialed it for this shot and the bar/cursor are still showing that.
void reset_post_form() {
  s_stars_value     = 0;
  s_taste_flags     = 0;
  s_delta.touched   = false;
  s_delta.velocity  = 0.0f;
  // Seed the delta bar at the preset's expected brew time so the cursor lands
  // on the user's normal target. They drag from there to their actual time.
  const auto p = presets::get(presets::selected_id());
  s_delta.value = std::clamp(static_cast<float>(p.target_time_s),
                             kDeltaSpec.min, kDeltaSpec.max);
  if (s_delta.widget != nullptr) lv_obj_invalidate(s_delta.widget);
  refresh_stars();
  refresh_taste_toggles();
  refresh_brew_time_labels();
  refresh_submit_enabled();
}

void enter_idle() {
  s_mode = Mode::Idle;
  reset_post_form();
  apply_mode();
}

void enter_post() {
  s_mode = Mode::Post;
  // Re-seed on entry too — preset may have been cycled in idle since the last
  // post session, and the delta bar's default needs to follow the new target.
  reset_post_form();
  refresh_preset_post_label();
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
  s_grind.value = std::clamp(presets::last_grind(new_id), kGrindMin, kGrindMax);
  refresh_preset_label();
  refresh_grind_value_label();
  refresh_suggestion();
  refresh_grinder();
}

void on_post_tap(lv_event_t*) {
  enter_post();
}

void on_cancel_tap(lv_event_t*) {
  enter_idle();
}

void on_star_tap(lv_event_t* e) {
  const auto idx = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
  if (s_stars_value == idx + 1) s_stars_value = static_cast<uint8_t>(idx);
  else                          s_stars_value = static_cast<uint8_t>(idx + 1);
  refresh_stars();
  refresh_submit_enabled();
}

// Sour / Bitter pills share a handler — the mask to flip is passed via
// user_data. XOR'd into s_taste_flags so a second tap clears the bit.
void on_taste_tap(lv_event_t* e) {
  const auto mask = static_cast<uint8_t>(
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
  s_taste_flags ^= mask;
  refresh_taste_toggles();
  // No submit-enabled change here — taste flags are journal-only and don't
  // gate Submit.
}

void on_submit(lv_event_t*) {
  if (!(s_stars_value > 0 && s_delta.touched)) return;  // belt-and-suspenders

  const climate::Reading r = climate::latest();
  storage::ShotRecord rec = {};
  rec.preset_id       = presets::selected_id();
  // Snap the floating bar value to a whole second, then subtract the preset's
  // expected target to get the stored delta. int8 clamp guards against the
  // theoretical 0 vs target=99 case (delta = -99); realistic deltas land
  // comfortably inside [-30, +30].
  {
    const auto p = presets::get(presets::selected_id());
    const int   snapped_s = static_cast<int>(std::lround(s_delta.value));
    const int   delta     = std::clamp(snapped_s -
                                       static_cast<int>(p.target_time_s),
                                       -128, 127);
    rec.time_delta_s = static_cast<int8_t>(delta);
  }
  rec.quality_stars   = s_stars_value;
  rec.taste_flags     = s_taste_flags;
  rec.timestamp_us    = esp_timer_get_time();
  rec.rtc_epoch_s     = rtc::epoch_s();
  rec.user_grind      = s_grind.value;
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
  refresh_grinder();
}

// ---------------------------------------------------------------------------
// Generic bar mechanics. Each BarState owns its own value, drag bookkeeping,
// and momentum timer. Hit-test, drag advance, momentum flick, and settle-snap
// are uniform — grind vs delta differs only in BarSpec values and on_change /
// on_settle / on_touched hooks. on_grind_bar_event and on_delta_bar_event are
// thin forwarders the overlay registers; everything below them is shared.
// ---------------------------------------------------------------------------
void bar_cancel_momentum(BarState* s) {
  if (s->momentum_timer != nullptr) {
    lv_timer_delete(s->momentum_timer);
    s->momentum_timer = nullptr;
  }
  s->velocity = 0.0f;
  s->momentum_ticks_left = 0;
}

void bar_snap_and_settle(BarState* s) {
  const float snapped = std::round(s->value / s->spec->step) * s->spec->step;
  if (snapped != s->value) {
    s->value = snapped;
    if (s->on_change) s->on_change(s);
  }
  s->velocity = 0.0f;
  if (s->on_settle) s->on_settle(s);
}

void bar_momentum_tick(lv_timer_t* t) {
  auto* s = static_cast<BarState*>(lv_timer_get_user_data(t));
  if (s == nullptr || s->spec == nullptr) return;
  const BarSpec& spec = *s->spec;
  const float dt_s = static_cast<float>(kMomentumPeriodMs) / 1000.0f;
  const float new_value =
      std::clamp(s->value + s->velocity * dt_s, spec.min, spec.max);
  if (new_value != s->value) {
    s->value = new_value;
    if (s->on_change) s->on_change(s);
  }
  s->velocity *= kMomentumDecay;
  --s->momentum_ticks_left;

  // Stop on any of: out of ticks, decayed below noise floor, or pinned to a
  // range edge. Snap + persist before the timer self-destructs.
  const bool at_edge = (s->value <= spec.min + 1e-4f) ||
                       (s->value >= spec.max - 1e-4f);
  if (s->momentum_ticks_left <= 0 ||
      std::fabs(s->velocity) < kMomentumMinSpeed ||
      at_edge) {
    bar_snap_and_settle(s);
    s->momentum_timer = nullptr;
    lv_timer_delete(t);
  }
}

void bar_dispatch_event(lv_event_t* e, BarState* s) {
  if (s == nullptr || s->spec == nullptr) return;
  const BarSpec& spec = *s->spec;
  const auto code = lv_event_get_code(e);
  lv_indev_t* indev = lv_indev_active();
  if (indev == nullptr) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);

  switch (code) {
    case LV_EVENT_PRESSED: {
      // Only react when the press lands in THIS bar's y-band. Outside the
      // band the bar stays asleep so the other bar (or no bar at all) can
      // claim the press.
      if (p.y < spec.y_band_top || p.y > spec.y_band_bottom) {
        s->dragging = false;
        return;
      }
      // Grabbing again mid-glide stops the flywheel — the new touch should
      // own the motion, not fight a tail from the last release.
      bar_cancel_momentum(s);
      s->dragging = true;
      s->last_x   = p.x;
      s->last_us  = esp_timer_get_time();
      // First touch in this form session — fire the hook so the Post form can
      // enable Submit. Subsequent touches don't re-fire (touched is sticky).
      if (!s->touched) {
        s->touched = true;
        if (s->on_touched) s->on_touched(s);
      }
      break;
    }
    case LV_EVENT_PRESSING: {
      if (!s->dragging) return;
      const int32_t dx_px = p.x - s->last_x;
      s->last_x = p.x;
      // Direct manipulation: the bar follows the finger. Finger right
      // (positive dx) scrolls ticks RIGHT under the fixed center cursor,
      // which reads a LOWER value — hence the sign flip on dv.
      //
      // Sub-step motion is fine; readout labels round for display in their
      // own refresh. Snap happens at release / momentum-end.
      const float px_per_unit =
          static_cast<float>(kBarHalfWidth) / spec.visible_half_range;
      const float dv = -static_cast<float>(dx_px) / px_per_unit;
      const float new_value =
          std::clamp(s->value + dv, spec.min, spec.max);
      if (new_value != s->value) {
        s->value = new_value;
        if (s->on_change) s->on_change(s);
      }
      // Track velocity for the post-release flick. EMA with α=0.5 smooths
      // single-frame jitter while still responding within ~2-3 frames.
      const uint64_t now_us = esp_timer_get_time();
      if (s->last_us != 0) {
        const float dt_s = static_cast<float>(now_us - s->last_us) / 1e6f;
        if (dt_s > 1e-4f) {
          const float instant = dv / dt_s;
          s->velocity = 0.5f * s->velocity + 0.5f * instant;
        }
      }
      s->last_us = now_us;
      break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: {
      if (!s->dragging) break;
      s->dragging = false;
      s->last_us  = 0;
      // If the finger lifted with non-trivial speed, hand off to the momentum
      // timer; it'll snap + settle when it dies. Otherwise settle now.
      if (std::fabs(s->velocity) >= kMomentumMinSpeed) {
        s->momentum_ticks_left = kMomentumMaxTicks;
        if (s->momentum_timer == nullptr) {
          s->momentum_timer =
              lv_timer_create(bar_momentum_tick, kMomentumPeriodMs, s);
        }
      } else {
        bar_snap_and_settle(s);
      }
      break;
    }
    default:
      break;
  }
}

// Per-bar forwarders. The grind overlay is always live; the delta overlay
// gates on mode so a press in post-mode coords during idle doesn't fire it.
void on_grind_bar_event(lv_event_t* e) {
  bar_dispatch_event(e, &s_grind);
}

void on_delta_bar_event(lv_event_t* e) {
  if (s_mode != Mode::Post) return;
  bar_dispatch_event(e, &s_delta);
}

// Per-bar hooks invoked by bar_dispatch_event / bar_momentum_tick when value
// changes, the drag/glide settles, or the bar is touched for the first time
// in a form session.
void grind_on_change(BarState*) {
  refresh_grinder();
  refresh_grind_value_label();
}
void grind_on_settle(BarState*) {
  presets::set_last_grind(presets::selected_id(), s_grind.value);
}

void delta_on_change(BarState* s) {
  if (s->widget != nullptr) lv_obj_invalidate(s->widget);
  // BREW TIME readout tracks the bar value frame-by-frame — same pattern as
  // grind_on_change → refresh_grind_value_label.
  refresh_brew_time_labels();
}
void delta_on_touched(BarState*) {
  refresh_submit_enabled();
}

// ---------------------------------------------------------------------------
// Climate tiles — geometry, icon drawing, value formatting, tap-to-toggle.
// ---------------------------------------------------------------------------
// Climate strip caps the top of the screen. The 3-column split is geometric
// thirds; separators land on those thirds in screen coords. Tile content
// (icon, label, value) is centered on each column — with one wrinkle: the
// round-display chord at the icon y is narrower than the screen, so the
// outer columns shift content inward by kOuterContentShift to avoid the
// round-clipped edge. Separators stay on the geometric thirds.
constexpr int32_t kClimateBottomY  = 210;
constexpr int32_t kColLeftEdge0    = 0;
constexpr int32_t kColLeftEdge1    = 155;
constexpr int32_t kColLeftEdge2    = 311;
constexpr int32_t kColRightEdge2   = kScreen;
// Icon + label rows sit higher than the value row to balance against the
// round-clip chord. Icon widget bounds oversize the visible glyph so the
// thermometer's cap arc and the drop's body circle aren't masked by
// LV_EVENT_DRAW_MAIN's layer.clip_area (set to the widget rect).
constexpr int32_t kTileIconY       = 46;    // top y of icon widget (all three)
constexpr int32_t kTileLabelY      = 108;   // top y of section caption
constexpr int32_t kTileValueY      = 130;   // top y of value (Montserrat 46)
constexpr int32_t kIconSize        = 60;
// Each separator line is pulled in by this much from both endpoints — the
// verticals stop short of the icon row, the horizontal stops short of the
// screen edge. Reads as quiet dividers rather than a rigid table-cell grid.
constexpr int32_t kSeparatorInset  = 10;
// Outer-tile content shifts *outward* from the tile center so pressure reads
// against the left edge and humidity against the right.
constexpr int32_t kOuterContentShift = -5;

constexpr int32_t kSeparatorThickness = 2;

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

  // Stroke width scaled to the icon size — a thinner stroke reads as spindly.
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

  auto line = [&](int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    ld.p1.x = x1; ld.p1.y = y1;
    ld.p2.x = x2; ld.p2.y = y2;
    lv_draw_line(layer, &ld);
  };

  // Boot-intro lerp factor. At 0 each icon draws its "empty" pose; at 1 it
  // draws `dynamic` verbatim. The anim runs once on first valid climate
  // reading, so steady-state redraws always see progress == 1.
  const float progress = std::clamp(state->intro_progress, 0.0f, 1.0f);

  // Per-kind icon geometry. Coordinates are integer-pixel-aligned for the
  // current kIconSize; if you change kIconSize, every numeric offset below
  // needs a re-tune (each branch is hand-fitted to the widget bounds).
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
      // Intro lerp sweeps the needle from parked-left (−90°) to the target
      // reading, so power-on reads as an "odometer-hand" wipe across the dial.
      constexpr float kNeedleLen = 18.0f;
      const float angle_deg = -90.0f + (state->dynamic - (-90.0f)) * progress;
      const float ang = angle_deg * (3.14159265f / 180.0f);
      const int32_t tx = cx + static_cast<int32_t>(kNeedleLen * std::sin(ang));
      const int32_t ty = cy + 13 - static_cast<int32_t>(kNeedleLen * std::cos(ang));
      line(cx, cy + 13, tx, ty);
      break;
    }
    case kIconThermo: {
      // Thermometer = outline shell + static mercury reservoir + dynamic
      // column. The shell is a 300° bulb arc (open at the top so the stem
      // verticals connect cleanly) + two stem lines + a 180° cap arc.
      //
      // Bulb arc endpoints (cx±5, cy+3) are where the stem verticals meet
      // the bulb circle: solving x²+y²=100 with x=±5 gives y=∓√75≈∓8.66,
      // so the join sits at (cy+12)−8.66 ≈ cy+3.34, rounded to cy+3. Arc
      // sweeps CW from 300° (upper-right join) through 90° (bulb bottom)
      // to 240°+360°=600° (upper-left join) — 300° of arc, leaving a 60°
      // opening at the top for the stem.
      ad.center.x    = cx;
      ad.center.y    = cy + 12;
      ad.radius      = 10;
      ad.start_angle = 300;
      ad.end_angle   = 600;
      lv_draw_arc(layer, &ad);
      line(cx - 5, cy - 18, cx - 5, cy + 3);
      line(cx + 5, cy - 18, cx + 5, cy + 3);
      ad.center.x    = cx;
      ad.center.y    = cy - 17;
      ad.radius      = 6;
      ad.start_angle = 200;
      ad.end_angle   = 340;
      lv_draw_arc(layer, &ad);

      // Static inner disc — 4 px gap to the bulb outline (r=6 inside the
      // r=10 outer), filled in thermo color so it reads as the mercury
      // reservoir even at 0 °F when the column collapses to nothing.
      lv_draw_rect_dsc_t fillc;
      lv_draw_rect_dsc_init(&fillc);
      fillc.bg_opa       = LV_OPA_COVER;
      fillc.bg_color     = state->color;
      fillc.border_width = 0;
      fillc.radius       = LV_RADIUS_CIRCLE;
      lv_area_t disc = {cx - 4, cy + 8, cx + 3, cy + 15};
      lv_draw_rect(layer, &fillc, &disc);

      // Dynamic mercury column — rounded-corner rect rising from the
      // center of the disc up toward the cap inside-edge at 100%. Bottom
      // anchored inside the disc (cy+12) so the rounded bottom corners
      // are hidden under the disc; only the column emerging above the
      // disc top (cy+6) is visible. Height = 0 at 0 % means we skip the
      // rect entirely so 0 °F reads as "reservoir only".
      //
      // pct maps °F linearly 0..100 → 0..100 % (set by
      // refresh_climate_temp). The unit toggle (°F vs °C) is text-only;
      // the column always reads the underlying temperature. Intro lerp
      // scales the column from 0 → target so the mercury rises on boot.
      const float pct = std::clamp(state->dynamic, 0.0f, 100.0f) * progress;
      constexpr int32_t kColBotOff    = +12;   // col bottom y offset
      constexpr int32_t kColTopOff100 = -18;   // col top y offset at 100%
      const int32_t col_height_max = kColBotOff - kColTopOff100;  // 33
      const int32_t col_height = static_cast<int32_t>(std::lround(
          (pct / 100.0f) * col_height_max));
      if (col_height > 0) {
        lv_area_t col = {cx - 1, cy + kColBotOff - col_height,
                         cx + 1, cy + kColBotOff};
        lv_draw_rect(layer, &fillc, &col);
      }
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
      // don't leak into the next tile's draws. Intro lerp scales 0 → target
      // so the drop fills from empty on boot.
      const float pct = std::clamp(state->dynamic, 0.0f, 100.0f) * progress;
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

  // Drive the mercury column from raw temp in °F regardless of the
  // displayed unit. Scale is centered on a room-temp reference: 20 °F →
  // 0 %, 70 °F → 50 %, 120 °F → 100 % (linear, clamped). The 100 °F
  // span keeps everyday readings (~60–80 °F) near the middle of the
  // stem, so cold and hot rooms both produce visible swing instead of
  // pinning the column at one end. The unit toggle (°F vs °C) only
  // affects the numeric readout — the column always reads the
  // underlying °F. Only invalidate on a ≥1 % change so the icon
  // doesn't repaint on sub-percent drift.
  const float new_pct = std::clamp(climate::c_to_f(r.temp_c) - 20.0f, 0.0f, 100.0f);
  if (std::fabs(new_pct - t.icon_state.dynamic) >= 1.0f) {
    t.icon_state.dynamic = new_pct;
    lv_obj_invalidate(t.icon);
  }
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

// ---------------------------------------------------------------------------
// Climate strip boot-intro animation — single source of truth for timings.
// ---------------------------------------------------------------------------
// On power-on the strip wakes up in two sequential passes:
//
//   1. TEXT pass (starts at t=kIntroTextDelayMs) — each tile's value,
//      inline suffix, and newline subtext slide up from kIntroTextSlideY
//      px below their final Y *and* fade in from black to their final
//      color in parallel (same duration, same easing, same per-tile
//      stagger). Section captions stay static so the column headers
//      anchor the eye while the numbers fly in. The tile container is
//      non-scrollable with its default overflow clip, so labels at
//      final_y + kIntroTextSlideY sit outside the 210-px container
//      bounds and get masked until they cross the bottom edge. Each tile
//      takes kIntroTextDurationMs ms and is offset by
//      kIntroTextStaggerMs ms from the previous tile (left → right), so
//      the strip reads as a wave rather than a single jump.
//
//   2. ICON pass (starts at t=kIntroIconDelayMs) — one anim ramps a 0..1
//      progress factor across all three ClimateIconState entries
//      simultaneously; the draw routine lerps each icon from its empty
//      pose (needle parked left, column at 0 %, drop drained) to its live
//      `dynamic` reading. Runs for kIntroIconDurationMs ms with no stagger.
//      Sequenced after the text slide so the numbers settle first and the
//      icons then bring them to life — feels like the dial reacting to
//      the readout rather than racing it.
constexpr uint32_t kIntroTextDelayMs    = 250;
constexpr uint32_t kIntroIconDelayMs    = 500;
constexpr uint32_t kIntroTextDurationMs = 250;
constexpr uint32_t kIntroTextStaggerMs  = 125;
constexpr int32_t  kIntroTextSlideY     = 15;
constexpr uint32_t kIntroIconDurationMs = 1000;
// Grind value label rides the same slide+fade as a climate tile but kicks off
// almost immediately on boot — the user's current setting is the screen's
// anchor, so it leads the wake-up wave and the climate strip catches up.
constexpr uint32_t kIntroGrindDelayMs   = 150;

void climate_intro_anim_cb(void* /*var*/, int32_t v) {
  const float progress = static_cast<float>(v) / 1000.0f;
  for (auto& tile : s_climate_tiles) {
    if (tile.icon == nullptr) continue;
    tile.icon_state.intro_progress = progress;
    lv_obj_invalidate(tile.icon);
  }
}

void start_climate_icon_intro_anim() {
  lv_anim_t a;
  lv_anim_init(&a);
  // Var is just an identity for LVGL's anim de-dup; the cb ignores it and
  // updates all three tiles directly.
  lv_anim_set_var(&a, &s_climate_tiles);
  lv_anim_set_exec_cb(&a, climate_intro_anim_cb);
  lv_anim_set_values(&a, 0, 1000);
  lv_anim_set_duration(&a, kIntroIconDurationMs);
  lv_anim_set_delay(&a, kIntroIconDelayMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

// Slide one label up into its final Y from kIntroTextSlideY px below. We
// capture the obj's current y (set by build_climate_tile / position_value_block
// just before we run) as the final position, snap it down, then ease it back.
// Hidden labels (suffix/subtext can be empty depending on the unit toggle) are
// skipped — animating them would be a no-op visually but still cost an
// lv_anim slot.
void slide_label_in(lv_obj_t* obj, uint32_t delay_ms) {
  if (obj == nullptr) return;
  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
  const int32_t final_y = lv_obj_get_y(obj);
  const int32_t start_y = final_y + kIntroTextSlideY;
  // Snap to the slid-down start before LVGL gets a chance to flush a frame,
  // so the first paint already shows the label off-screen rather than
  // flickering through its final position.
  lv_obj_set_y(obj, start_y);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, reinterpret_cast<lv_anim_exec_xcb_t>(lv_obj_set_y));
  lv_anim_set_values(&a, start_y, final_y);
  lv_anim_set_duration(&a, kIntroTextDurationMs);
  lv_anim_set_delay(&a, delay_ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

// Per-target-color fade exec_cbs. The text fade runs in parallel with the
// slide, so we get a second concurrent lv_anim per label (LVGL keys anims by
// (var, exec_cb) — these have a different cb pointer than lv_obj_set_y, so
// they don't clobber the slide anim). v ranges 0..255: 0 = pure black,
// 255 = the label's final color. We need one cb per target because lv_anim's
// exec_cb takes (var, value) and there's no per-anim color slot to carry the
// target through; for two final colors (kColorText vs kColorMuted) it's
// cleaner to bind the color at the function level than to thread it through
// lv_anim_set_user_data.
void fade_text_to_text(void* var, int32_t v) {
  lv_obj_set_style_text_color(
      static_cast<lv_obj_t*>(var),
      lv_color_mix(kColorText, lv_color_black(), static_cast<uint8_t>(v)),
      LV_PART_MAIN);
}
void fade_text_to_muted(void* var, int32_t v) {
  lv_obj_set_style_text_color(
      static_cast<lv_obj_t*>(var),
      lv_color_mix(kColorMuted, lv_color_black(), static_cast<uint8_t>(v)),
      LV_PART_MAIN);
}

void fade_label_in(lv_obj_t* obj, lv_anim_exec_xcb_t cb, uint32_t delay_ms) {
  if (obj == nullptr) return;
  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
  // Snap to pure black before LVGL flushes so the first paint matches the
  // anim's starting frame.
  lv_obj_set_style_text_color(obj, lv_color_black(), LV_PART_MAIN);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, cb);
  lv_anim_set_values(&a, 0, 255);
  lv_anim_set_duration(&a, kIntroTextDurationMs);
  lv_anim_set_delay(&a, delay_ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

// Re-run the slide+fade combo for one tile's value/suffix/subtext. Called both
// from the boot intro (with per-tile stagger delay) and from the tap handlers
// (delay = 0) so a unit toggle replays the same wake-up motion against the
// freshly rendered text. Section caption (t.label) is intentionally untouched.
// Safe to call mid-flight: lv_anim_start replaces any existing anim with the
// same (var, exec_cb), and position_value_block has just snapped each label
// back to its true final Y, so lv_obj_get_y inside slide_label_in reads the
// correct destination even if a prior anim was still running.
void animate_tile_text_in(ClimateTile& t, uint32_t delay_ms) {
  if (t.container == nullptr) return;
  slide_label_in(t.value_lbl,   delay_ms);
  slide_label_in(t.suffix_lbl,  delay_ms);
  slide_label_in(t.subtext_lbl, delay_ms);
  fade_label_in(t.value_lbl,   fade_text_to_text,  delay_ms);
  fade_label_in(t.suffix_lbl,  fade_text_to_muted, delay_ms);
  fade_label_in(t.subtext_lbl, fade_text_to_muted, delay_ms);
}

void start_climate_text_intro_anim() {
  for (size_t i = 0; i < 3; i++) {
    const uint32_t delay =
        kIntroTextDelayMs + static_cast<uint32_t>(i) * kIntroTextStaggerMs;
    animate_tile_text_in(s_climate_tiles[i], delay);
  }
}

// translate_y based slide for labels positioned via lv_obj_align — unlike
// slide_label_in's lv_obj_set_y path, translate is purely visual and doesn't
// stack with the alignment offset on the next layout pass.
void translate_y_exec(void* var, int32_t v) {
  lv_obj_set_style_translate_y(static_cast<lv_obj_t*>(var), v, LV_PART_MAIN);
}

void slide_label_in_translate(lv_obj_t* obj, uint32_t delay_ms) {
  if (obj == nullptr) return;
  if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
  lv_obj_set_style_translate_y(obj, kIntroTextSlideY, LV_PART_MAIN);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, obj);
  lv_anim_set_exec_cb(&a, translate_y_exec);
  lv_anim_set_values(&a, kIntroTextSlideY, 0);
  lv_anim_set_duration(&a, kIntroTextDurationMs);
  lv_anim_set_delay(&a, delay_ms);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_start(&a);
}

void start_grind_value_intro_anim() {
  slide_label_in_translate(s_grind_value_label, kIntroGrindDelayMs);
  fade_label_in(s_grind_value_label, fade_text_to_text, kIntroGrindDelayMs);
}

void refresh_climate_tiles() {
  const climate::Reading r = climate::latest();
  refresh_climate_pressure(r);
  refresh_climate_temp(r);
  refresh_climate_humidity(r);

  // Kick the boot-intro anims once, the first time we land a real reading.
  // Before that, the BME280 task is still warming up and the tiles render
  // "--" with their icons parked at the empty pose (progress = 0). The text
  // anim must fire AFTER the refresh calls above, since slide_label_in reads
  // each label's freshly placed Y as the final destination.
  static bool intro_played = false;
  if (!intro_played && r.timestamp_us != 0) {
    intro_played = true;
    start_climate_icon_intro_anim();
    start_climate_text_intro_anim();
  }
}

void on_pressure_tap(lv_event_t*) {
  climate::set_pressure_unit(
      climate::pressure_unit() == climate::PressureUnit::InHg
          ? climate::PressureUnit::HPa
          : climate::PressureUnit::InHg);
  refresh_climate_pressure(climate::latest());
  animate_tile_text_in(s_climate_tiles[0], 0);
}

void on_temp_tap(lv_event_t*) {
  climate::set_temp_unit(climate::temp_unit() == climate::TempUnit::Fahrenheit
                             ? climate::TempUnit::Celsius
                             : climate::TempUnit::Fahrenheit);
  const climate::Reading r = climate::latest();
  refresh_climate_temp(r);
  // Dew-point readout follows the temperature unit, so flipping temp also
  // updates humidity when it's in dew-point mode. The humidity tile's value
  // is updated in place (no slide/fade replay) — only the directly-tapped
  // tile animates, so the °C↔°F-driven dew-point change reads as a quiet
  // side effect rather than a competing wave next to the tapped tile.
  refresh_climate_humidity(r);
  animate_tile_text_in(s_climate_tiles[1], 0);
}

void on_humidity_tap(lv_event_t*) {
  climate::set_humidity_unit(
      climate::humidity_unit() == climate::HumidityUnit::Percent
          ? climate::HumidityUnit::DewPoint
          : climate::HumidityUnit::Percent);
  refresh_climate_humidity(climate::latest());
  animate_tile_text_in(s_climate_tiles[2], 0);
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

// 5-point star polyline, 11 vertices (5 outer + 5 inner alternating, with
// the first vertex repeated at the end to close the polygon). Inner/outer
// ratio drives how chunky the arms read — smaller ratio gives the slim
// golden-ratio classic, larger ratio gives the bolder Font-Awesome-style
// filled look. Tune kStarInnerRatio when the stars feel too spindly or too
// puffy for the layout. All five Quality stars share the same size, so the
// polyline lives in a single static array that every star widget
// references; the draw handler translates it to layer coords per paint, so
// the array can be widget-relative and shared across positions.
constexpr int32_t kStarSize        = 38;
constexpr float   kStarInnerRatio  = 0.5f;
// Sub-pixel outward push applied to each triangle vertex (from the
// triangle's own centroid) so adjacent triangles overlap slightly along
// their shared edges. LVGL's AA rasterizer otherwise leaves seam pixels
// uncovered where two triangles meet — visible as faint lines. Bumping
// the perimeter outward by this fraction of a pixel is invisible at the
// star's size; the seam lines are not.
constexpr float   kStarFillOverlap = 0.5f;
lv_point_precise_t s_star_polyline[11];
bool s_star_polyline_built = false;

void build_star_polyline() {
  if (s_star_polyline_built) return;
  const float R = static_cast<float>(kStarSize) * 0.5f;
  const float r = R * kStarInnerRatio;
  for (int i = 0; i <= 10; ++i) {
    // i=0 is the top outer vertex; subsequent vertices step 36° clockwise,
    // alternating outer (even i) and inner (odd i). i=10 closes back to i=0.
    const float a = kPi / 2.0f - kPi * static_cast<float>(i % 10) / 5.0f;
    const float rad = (i % 2 == 0) ? R : r;
    s_star_polyline[i].x = R + rad * std::cos(a);
    s_star_polyline[i].y = R - rad * std::sin(a);
  }
  s_star_polyline_built = true;
}

void draw_star_event(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  if (layer == nullptr) return;
  auto* obj   = static_cast<lv_obj_t*>(lv_event_get_target(e));
  auto* state = static_cast<StarState*>(lv_obj_get_user_data(obj));
  if (state == nullptr) return;

  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  const auto ox = static_cast<lv_value_precise_t>(coords.x1);
  const auto oy = static_cast<lv_value_precise_t>(coords.y1);

  // Translate widget-relative polyline → layer-absolute coords for this paint.
  lv_point_precise_t pts[11];
  for (int i = 0; i < 11; ++i) {
    pts[i].x = s_star_polyline[i].x + ox;
    pts[i].y = s_star_polyline[i].y + oy;
  }

  if (state->lit) {
    // Fill the star as inner-pentagon + 5 arm triangles. A centroid fan
    // would converge 10 triangles at one sub-pixel, leaving a pinhole
    // LVGL's AA can't fill; this decomposition shares only full edges
    // between adjacent triangles.
    //
    // Vertex layout in pts[]: even indices (0,2,4,6,8) are outer tips,
    // odd indices (1,3,5,7,9) are the inner V's between tips.
    //
    // Each triangle is then pushed outward from its own centroid by
    // kStarFillOverlap so adjacent triangles overlap along their shared
    // edge — without this, LVGL's per-triangle AA coverage along the
    // shared edge sums to <100% and a faint seam line shows through.
    const lv_point_precise_t raw_tris[8][3] = {
      // Inner pentagon — fan from inner V at index 1 through the others.
      {pts[1], pts[3], pts[5]},
      {pts[1], pts[5], pts[7]},
      {pts[1], pts[7], pts[9]},
      // Arms: (prev inner V, outer tip, next inner V). Top tip's prev
      // inner V wraps from the polyline's end.
      {pts[9], pts[0], pts[1]},
      {pts[1], pts[2], pts[3]},
      {pts[3], pts[4], pts[5]},
      {pts[5], pts[6], pts[7]},
      {pts[7], pts[8], pts[9]},
    };

    lv_draw_triangle_dsc_t tri;
    lv_draw_triangle_dsc_init(&tri);
    tri.color = kColorAccent;
    tri.opa   = LV_OPA_COVER;
    for (const auto& src : raw_tris) {
      const lv_value_precise_t cx = (src[0].x + src[1].x + src[2].x) / 3.0f;
      const lv_value_precise_t cy = (src[0].y + src[1].y + src[2].y) / 3.0f;
      for (int v = 0; v < 3; ++v) {
        const lv_value_precise_t dx = src[v].x - cx;
        const lv_value_precise_t dy = src[v].y - cy;
        const float len = std::sqrt(
            static_cast<float>(dx * dx + dy * dy));
        if (len > 1e-6f) {
          tri.p[v].x = src[v].x + dx * kStarFillOverlap / len;
          tri.p[v].y = src[v].y + dy * kStarFillOverlap / len;
        } else {
          tri.p[v] = src[v];
        }
      }
      lv_draw_triangle(layer, &tri);
    }
  } else {
    // Outline: 10 stroke segments with rounded caps so the joints read as
    // continuous (matches the prior lv_line widget output).
    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color       = kColorMuted3;
    line.width       = 3;
    line.opa         = LV_OPA_COVER;
    line.round_start = 1;
    line.round_end   = 1;
    for (int i = 0; i < 10; ++i) {
      line.p1 = pts[i];
      line.p2 = pts[i + 1];
      lv_draw_line(layer, &line);
    }
  }
}

lv_obj_t* make_star(lv_obj_t* parent, StarState* state) {
  build_star_polyline();
  lv_obj_t* w = lv_obj_create(parent);
  lv_obj_set_size(w, kStarSize, kStarSize);
  lv_obj_set_style_bg_opa(w, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(w, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(w, 0, LV_PART_MAIN);
  lv_obj_clear_flag(w, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(w, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_user_data(w, state);
  lv_obj_add_event_cb(w, draw_star_event, LV_EVENT_DRAW_MAIN, nullptr);
  return w;
}

// ---------------------------------------------------------------------------
// Group builders.
// ---------------------------------------------------------------------------
// Make one bar's tick-strip widget. Parent decides visibility — the grind bar
// goes on the screen (visible both modes); the delta bar goes inside
// s_post_group so the mode swap hides it. The BarState's spec/widget/hooks
// must be wired by the caller before lv_obj_invalidate triggers a paint.
lv_obj_t* make_bar_widget(lv_obj_t* parent, BarState* state) {
  lv_obj_t* w = lv_obj_create(parent);
  lv_obj_set_size(w, 2 * kBarHalfWidth, kBarStripHeight);
  lv_obj_set_pos(w, kCenter - kBarHalfWidth,
                 state->spec->y - kBarStripHeight / 2);
  lv_obj_set_style_bg_opa(w, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(w, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(w, 0, LV_PART_MAIN);
  lv_obj_clear_flag(w, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(w, LV_OBJ_FLAG_CLICKABLE);
  // user_data carries the BarState* so draw_bar_event can pull the value /
  // spec without globals.
  lv_obj_set_user_data(w, state);
  lv_obj_add_event_cb(w, draw_bar_event, LV_EVENT_DRAW_MAIN, nullptr);
  return w;
}

void build_grinder(lv_obj_t* scr) {
  // Wire the grind bar state. Spec is constexpr; widget+value+hooks attach
  // here. (Delta bar is wired in build_post_group, on its own group.)
  s_grind.spec      = &kGrindSpec;
  s_grind.on_change = grind_on_change;
  s_grind.on_settle = grind_on_settle;
  s_grind.on_touched = nullptr;  // grind doesn't gate any UI on "touched"

  // Transparent full-screen overlay catches horizontal swipes for BOTH bars.
  // Per-bar forwarders hit-test against their own spec.y_band and ignore
  // events that don't belong to them. Sits BEHIND the climate tiles + Post
  // button + Post-mode buttons so those widgets take their own taps first.
  s_grinder_overlay = lv_obj_create(scr);
  lv_obj_set_size(s_grinder_overlay, kScreen, kScreen);
  lv_obj_set_pos(s_grinder_overlay, 0, 0);
  lv_obj_set_style_bg_opa(s_grinder_overlay, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_grinder_overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_grinder_overlay, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_grinder_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_grinder_overlay, LV_OBJ_FLAG_CLICKABLE);
  for (auto code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED,
                    LV_EVENT_PRESS_LOST}) {
    lv_obj_add_event_cb(s_grinder_overlay, on_grind_bar_event, code, nullptr);
    lv_obj_add_event_cb(s_grinder_overlay, on_delta_bar_event, code, nullptr);
  }

  // Tick bar — custom-drawn widget centered on (kCenter, kBarY). On every
  // drag, LVGL invalidates only this widget's bounds.
  s_grind.widget = make_bar_widget(scr, &s_grind);

  // Upward cursor — fixed at bar center, tip points UP at the bar from the
  // row below. Widget bounds (kCursorWidget) are larger than the triangle,
  // so we offset by half that slack to land the tip kCursorTipGap below the
  // big-tick extent of the bar.
  s_cursor_arrow_state.color = kColorText;
  s_static_cursor = make_arrow(scr, &s_cursor_arrow_state, kCursorWidget);
  const int32_t cursor_tip_y      = kBarY + kBigTickLen / 2 + kCursorTipGap;
  const int32_t cursor_tip_inset  = (kCursorWidget - kCursorArrowHeight) / 2;
  lv_obj_set_pos(s_static_cursor,
                 kCenter - kCursorWidget / 2,
                 cursor_tip_y - cursor_tip_inset);

  // Suggestion arrow — same widget host + same tip y as the cursor, slightly
  // shorter so when the model's grind matches the user's the cursor still
  // peeks out underneath. Created AFTER the cursor so LVGL paints it on top.
  // Position + color updated each refresh_suggestion_arrow() call; hidden
  // initially until the first refresh decides there's something to show.
  s_suggestion_arrow = make_arrow(scr, &s_suggestion_arrow_state, kCursorWidget);
  lv_obj_add_flag(s_suggestion_arrow, LV_OBJ_FLAG_HIDDEN);

  // Current value as big text above the bar, visible in BOTH modes. Suggested
  // line tucks between this and the bar (further down).
  s_grind_value_label = lv_label_create(scr);
  lv_obj_set_style_text_color(s_grind_value_label, kColorText, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_grind_value_label, &lv_font_montserrat_48,
                             LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_grind_value_label, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_align(s_grind_value_label, LV_ALIGN_CENTER, 0, kGrindValueY);
  lv_obj_clear_flag(s_grind_value_label, LV_OBJ_FLAG_CLICKABLE);

  // Static caption to the left of the value, same muted-gray + Montserrat 14
  // treatment as the climate section captions so the eye reads it as a header
  // for the big number rather than another live readout.
  s_grind_caption = lv_label_create(scr);
  lv_obj_set_style_text_color(s_grind_caption, kColorLabel, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_grind_caption, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(s_grind_caption, "GRIND VALUE");
  lv_obj_set_pos(s_grind_caption, kGrindCaptionX, kGrindCaptionY);

  // SUGGESTION block — mirrors the GRIND VALUE caption on the right side of
  // the big number. Caption is the muted-gray static header; the value line
  // underneath carries the live number and confidence-tier color. Both share
  // a fixed-width box with centered text so the second line stays anchored
  // to the caption regardless of digit count. Both hidden until the model
  // produces a usable suggestion; refresh_suggested_label toggles them as a
  // pair so the caption never floats over an empty value row.
  s_suggestion_caption = lv_label_create(scr);
  lv_obj_set_style_text_color(s_suggestion_caption, kColorLabel, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_suggestion_caption, &lv_font_montserrat_14,
                             LV_PART_MAIN);
  lv_label_set_text(s_suggestion_caption, "SUGGESTION");
  lv_obj_set_pos(s_suggestion_caption, kSuggestionBlockX, kSuggestionCaptionY);
  lv_obj_set_width(s_suggestion_caption, kSuggestionBlockW);
  lv_obj_set_style_text_align(s_suggestion_caption, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_add_flag(s_suggestion_caption, LV_OBJ_FLAG_HIDDEN);

  s_suggested_label = lv_label_create(scr);
  lv_obj_set_style_text_color(s_suggested_label, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_suggested_label, &lv_font_montserrat_14,
                             LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_suggested_label, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_pos(s_suggested_label, kSuggestionBlockX, kSuggestionValueY);
  lv_obj_set_width(s_suggested_label, kSuggestionBlockW);
  lv_obj_set_style_text_align(s_suggested_label, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_clear_flag(s_suggested_label, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_suggested_label, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t* make_separator(lv_obj_t* parent, int32_t x, int32_t y,
                         int32_t w, int32_t h) {
  lv_obj_t* s = lv_obj_create(parent);
  lv_obj_set_size(s, w, h);
  lv_obj_set_pos(s, x, y);
  lv_obj_set_style_bg_color(s, kColorMuted4, LV_PART_MAIN);
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
  lv_obj_set_style_text_color(t.label, kColorLabel, LV_PART_MAIN);
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
  // Group container itself is non-clickable so taps fall through to the
  // grinder swipe overlay unless they hit a child widget.
  lv_obj_clear_flag(s_idle_group, LV_OBJ_FLAG_CLICKABLE);

  // --- Climate area ---
  // Two vertical separators on the geometric thirds (idle-only — they segment
  // climate columns and hide when the top area swaps to post UI). The
  // horizontal separator below the climate strip (at kClimateBottomY) is
  // parented to `scr` instead so it stays visible across both modes — it sits
  // above the center line and the user wants it preserved in post too.
  make_separator(s_idle_group, kColLeftEdge1 - kSeparatorThickness / 2, kSeparatorInset,
                 kSeparatorThickness, kClimateBottomY - 2 * kSeparatorInset);
  make_separator(s_idle_group, kColLeftEdge2 - kSeparatorThickness / 2, kSeparatorInset,
                 kSeparatorThickness, kClimateBottomY - 2 * kSeparatorInset);
  make_separator(scr, kSeparatorInset, kClimateBottomY - kSeparatorThickness / 2,
                 kScreen - 2 * kSeparatorInset, kSeparatorThickness);

  build_climate_tile(s_idle_group, 0, kColLeftEdge0,  kColLeftEdge1,
                     kIconGauge,  kColorIconPressure, "PRESSURE",
                     on_pressure_tap);
  build_climate_tile(s_idle_group, 1, kColLeftEdge1,  kColLeftEdge2,
                     kIconThermo, kColorIconTemp,    "TEMPERATURE",
                     on_temp_tap);
  build_climate_tile(s_idle_group, 2, kColLeftEdge2,  kColRightEdge2,
                     kIconDrop,   kColorIconHumidity,   "HUMIDITY",
                     on_humidity_tap);

  // (Grind value sits above the bar; it's created in build_grinder as an
  // always-visible widget so it survives the idle→post toggle.)

  // Post button — opens the post-mode form. Lives in s_idle_group so it hides
  // when entering post (post mode replaces the center line with ✕ / preset /
  // Submit).
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

  // Horizontal separator capping the grinder area. Parented to `scr` so it
  // stays visible across both modes — the bottom (grind) area below it is
  // identical in idle and post.
  make_separator(scr, kSeparatorInset,
                 kGrinderSeparatorY - kSeparatorThickness / 2,
                 kScreen - 2 * kSeparatorInset, kSeparatorThickness);

  // Preset selector — small tap-to-cycle label parked at the vertical middle
  // of the screen, just left of POST. Lives in s_idle_group; the post mode
  // shows a read-only preset string in its place. The button auto-sizes to
  // its label, so refresh_preset_label re-runs the LV_ALIGN_OUT_LEFT_MID
  // alignment after every text change — otherwise it overflows rightward as
  // the label grows.
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

  // Top-area layout (replaces climate strip in idle). Mirror image of the
  // grind area below: cursor at the TOP pointing down, bar below the cursor,
  // BREW TIME row (caption / value / DELTA block) below the bar. The
  // climate-bottom separator at y=210 stays visible (parented to scr) and
  // caps the post form. The center line at y=253 swaps from POST/preset
  // (idle) to ✕/preset readonly/Submit (post). Below y=210 — center line,
  // cap separator at y=295, big grind number, GRIND VALUE caption,
  // SUGGESTION block, grind bar — is unchanged across both modes.
  //
  //   y= 54  delta cursor tip (downward arrow, mirror of grind cursor)
  //   y= 82  delta bar (kDeltaBarY; band [0, 100])
  //   y=115  BREW TIME caption (left, x=50) · value "30s" Mont 36 (center,
  //          glyph row y=96–134) · DELTA + "+/-Ns" two-line block (right)
  //   y=153  QUALITY caption (left, x=50, vert-centered on stars) ·
  //          5-star row (kStarSize=38) · Sour/Bitter pill stack (right)
  //   y=210  ── separator (shared) ──
  //   y=253  ✕  preset readonly  Submit
  //
  // The stars row and the pill stack share the same vertical band — the
  // pills are stacked so their column avg-Y matches the star row center
  // Y, reading as a single horizontal "rate this shot" tier.

  // --- Downward cursor at the top, pointing at the bar below ---
  // draw_arrow_event's default orientation puts the tip at (0, +h/2) — i.e.
  // h/2 BELOW the widget center for a 180° (downward) arrow. To land the
  // tip on kDeltaCursorTipY, the widget center must sit h/2 above it, so the
  // widget top edge is (h + W) / 2 above the tip.
  constexpr int32_t kDeltaCursorTipY = kDeltaBarY - kBigTickLen / 2 - kCursorTipGap;
  s_delta_cursor = make_arrow(s_post_group, &s_delta_cursor_state, kCursorWidget);
  lv_obj_set_pos(s_delta_cursor,
                 kCenter - kCursorWidget / 2,
                 kDeltaCursorTipY - (kCursorArrowHeight + kCursorWidget) / 2);

  // --- Delta bar ---
  // Wire state, then create the widget INSIDE the post group so the mode swap
  // hides it. The shared overlay still owns touch dispatch; on_delta_bar_event
  // filters by Mode::Post + spec.y_band.
  s_delta.spec       = &kDeltaSpec;
  s_delta.on_change  = delta_on_change;
  s_delta.on_settle  = nullptr;             // delta persists at submit time, not per-glide
  s_delta.on_touched = delta_on_touched;
  s_delta.widget     = make_bar_widget(s_post_group, &s_delta);

  // --- BREW TIME row (caption left, value center, DELTA block right) ---
  // Mirrors the grind readout's GRIND VALUE caption + big number + SUGGESTION
  // block, just with smaller font (Mont 36 vs Mont 48) since the delta is the
  // secondary readout. Caption x=50 and DELTA block x=302 reuse the grind
  // side's column positions so the eye reads both as the same layout.
  constexpr int32_t kBrewTimeValueCenterY = 115;
  constexpr int32_t kBrewCaptionTopY      = kBrewTimeValueCenterY - 7;
  constexpr int32_t kBrewDeltaCaptionY    = kBrewTimeValueCenterY - 16;
  constexpr int32_t kBrewDeltaValueTopY   = kBrewTimeValueCenterY + 2;

  s_brew_time_caption = lv_label_create(s_post_group);
  lv_obj_set_style_text_color(s_brew_time_caption, kColorLabel, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_brew_time_caption, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(s_brew_time_caption, "BREW TIME");
  // Visually anchor BREW TIME under the same horizontal column center as
  // GRIND VALUE. Both are auto-sized labels with different widths, so a
  // shared left edge (kGrindCaptionX) would leave their centers misaligned —
  // measure GRIND VALUE's width and offset BREW TIME so its center matches.
  // build_grinder runs before build_post_group, so s_grind_caption has been
  // text-set + auto-sized; update_layout makes the width query reliable
  // before LVGL's own layout pass at the end of start_report.
  lv_obj_update_layout(s_grind_caption);
  lv_obj_update_layout(s_brew_time_caption);
  const int32_t grind_caption_cx =
      kGrindCaptionX + lv_obj_get_width(s_grind_caption) / 2;
  const int32_t brew_caption_x =
      grind_caption_cx - lv_obj_get_width(s_brew_time_caption) / 2;
  lv_obj_set_pos(s_brew_time_caption, brew_caption_x, kBrewCaptionTopY);

  s_brew_time_value = lv_label_create(s_post_group);
  lv_obj_set_style_text_color(s_brew_time_value, kColorText, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_brew_time_value, &lv_font_montserrat_36, LV_PART_MAIN);
  // Centered horizontally at screen center; vertical via offset from
  // LV_ALIGN_TOP_MID puts the glyph row center near kBrewTimeValueCenterY.
  // (Mont 36 glyph height ≈ 38; shift up by half so center lands on Y.)
  lv_obj_align(s_brew_time_value, LV_ALIGN_TOP_MID, 0,
               kBrewTimeValueCenterY - 19);
  lv_label_set_text(s_brew_time_value, "0s");

  s_brew_delta_caption = lv_label_create(s_post_group);
  lv_obj_set_style_text_color(s_brew_delta_caption, kColorLabel, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_brew_delta_caption, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(s_brew_delta_caption, "DELTA");
  lv_obj_set_pos(s_brew_delta_caption, kSuggestionBlockX, kBrewDeltaCaptionY);
  lv_obj_set_width(s_brew_delta_caption, kSuggestionBlockW);
  lv_obj_set_style_text_align(s_brew_delta_caption, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

  s_brew_delta_value = lv_label_create(s_post_group);
  lv_obj_set_style_text_font(s_brew_delta_value, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_pos(s_brew_delta_value, kSuggestionBlockX, kBrewDeltaValueTopY);
  lv_obj_set_width(s_brew_delta_value, kSuggestionBlockW);
  lv_obj_set_style_text_align(s_brew_delta_value, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_text(s_brew_delta_value, "0s");

  // --- QUALITY caption + 5-star row (left/center) and taste pill stack
  //     (right), all sharing the band between the BREW TIME row and the
  //     y=210 separator. Pill stack and star row are vertically centered
  //     on the same Y so the whole "rate this shot" tier reads as one
  //     horizontal row, regardless of the stack being taller than the
  //     stars.
  constexpr int32_t kStarGap            = 8;
  constexpr int32_t kStarRowY           = 153;
  constexpr int32_t kStarRowX           = 115;
  constexpr int32_t kQualityCaptionX    = kGrindCaptionX;
  constexpr int32_t kPillH              = 28;
  constexpr int32_t kPillW              = 88;
  constexpr int32_t kPillStackVGap      = 8;
  constexpr int32_t kPillStackRightInset = 30;
  constexpr int32_t kPillStackX         =
      kScreen - kPillStackRightInset - kPillW;
  constexpr int32_t kPillStackTopY      = 140;
  static_assert(kStarRowY + kStarSize / 2 ==
                kPillStackTopY + kPillH + kPillStackVGap / 2,
                "star row center Y must match pill stack avg Y");

  // QUALITY caption — vertically centered on the star row. lv_label is
  // auto-sized; measuring after update_layout gives us the rendered height
  // so the caption sits on the star's vertical midline.
  s_quality_caption = lv_label_create(s_post_group);
  lv_obj_set_style_text_color(s_quality_caption, kColorLabel, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_quality_caption, &lv_font_montserrat_14,
                             LV_PART_MAIN);
  lv_label_set_text(s_quality_caption, "QUALITY");
  lv_obj_update_layout(s_quality_caption);
  const int32_t quality_caption_y =
      kStarRowY + (kStarSize - lv_obj_get_height(s_quality_caption)) / 2;
  lv_obj_set_pos(s_quality_caption, kQualityCaptionX, quality_caption_y);

  for (uint8_t i = 0; i < kMaxStars; ++i) {
    lv_obj_t* tap = lv_obj_create(s_post_group);
    lv_obj_set_size(tap, kStarSize, kStarSize);
    lv_obj_set_pos(tap, kStarRowX + i * (kStarSize + kStarGap), kStarRowY);
    lv_obj_set_style_bg_opa(tap, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(tap, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(tap, 0, LV_PART_MAIN);
    lv_obj_clear_flag(tap, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tap, on_star_tap, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
    s_star_btns[i]  = tap;
    s_star_icons[i] = make_star(tap, &s_star_states[i]);
  }

  // --- Sour / Bitter toggle pills, stacked vertically on the right ---
  // The stack right-aligns with Submit (same kCenterEdgeInset == 30 from
  // the screen edge) so Submit and the pills share a vertical column.
  struct PillCfg { const char* text; uint8_t mask; lv_obj_t** btn; lv_obj_t** lbl; };
  PillCfg pills[2] = {
      {"Sour",   storage::kTasteSour,   &s_sour_btn,   &s_sour_label},
      {"Bitter", storage::kTasteBitter, &s_bitter_btn, &s_bitter_label},
  };
  for (int i = 0; i < 2; ++i) {
    lv_obj_t* b = lv_button_create(s_post_group);
    lv_obj_set_size(b, kPillW, kPillH);
    lv_obj_set_style_radius(b, kPillH / 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, 0, LV_PART_MAIN);
    lv_obj_set_pos(b, kPillStackX,
                   kPillStackTopY + i * (kPillH + kPillStackVGap));
    lv_obj_add_event_cb(b, on_taste_tap, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(
                            static_cast<uintptr_t>(pills[i].mask)));
    lv_obj_t* lbl = lv_label_create(b);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_label_set_text(lbl, pills[i].text);
    lv_obj_center(lbl);
    *pills[i].btn = b;
    *pills[i].lbl = lbl;
  }

  // --- Center line: ✕ (left), preset readonly (mid), Submit (right) ---
  // y_offset = 20 matches the idle POST button's LV_ALIGN_CENTER offset so
  // the center line sits on the same horizontal as idle's POST. Mode swap
  // replaces idle's POST + preset_btn with this triplet at the same y.
  constexpr int32_t kCenterRowOffsetY = 20;
  constexpr int32_t kCenterEdgeInset  = 30;
  constexpr int32_t kCancelSize       = 40;
  constexpr int32_t kSubmitW          = 100;
  constexpr int32_t kSubmitH          = 44;

  s_cancel_btn = lv_button_create(s_post_group);
  lv_obj_set_size(s_cancel_btn, kCancelSize, kCancelSize);
  lv_obj_set_style_radius(s_cancel_btn, kCancelSize / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_cancel_btn, kColorMuted3, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_cancel_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_cancel_btn, 0, LV_PART_MAIN);
  lv_obj_align(s_cancel_btn, LV_ALIGN_LEFT_MID, kCenterEdgeInset,
               kCenterRowOffsetY);
  lv_obj_add_event_cb(s_cancel_btn, on_cancel_tap, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* cancel_lbl = lv_label_create(s_cancel_btn);
  lv_obj_set_style_text_color(cancel_lbl, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(cancel_lbl, LV_SYMBOL_CLOSE);
  lv_obj_center(cancel_lbl);

  // Read-only preset string, same format as the idle preset button. Centered
  // between ✕ and Submit. Not tappable — cycling presets mid-form would
  // invalidate the delta bar's seeded default.
  s_preset_post_label = lv_label_create(s_post_group);
  lv_obj_set_style_text_color(s_preset_post_label, kColorMuted, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_preset_post_label, &lv_font_montserrat_14,
                             LV_PART_MAIN);
  lv_obj_align(s_preset_post_label, LV_ALIGN_CENTER, 0, kCenterRowOffsetY);

  s_submit_btn = lv_button_create(s_post_group);
  lv_obj_set_size(s_submit_btn, kSubmitW, kSubmitH);
  lv_obj_set_style_radius(s_submit_btn, kSubmitH / 2, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_submit_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_submit_btn, 0, LV_PART_MAIN);
  lv_obj_align(s_submit_btn, LV_ALIGN_RIGHT_MID, -kCenterEdgeInset,
               kCenterRowOffsetY);
  s_submit_label = lv_label_create(s_submit_btn);
  lv_obj_set_style_text_font(s_submit_label, &lv_font_montserrat_24,
                             LV_PART_MAIN);
  lv_label_set_text(s_submit_label, "Submit");
  lv_obj_center(s_submit_label);
  lv_obj_add_event_cb(s_submit_btn, on_submit, LV_EVENT_CLICKED, nullptr);
}

}  // namespace

void start_report() {
  if (!display::lock()) return;

  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, kColorBg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

  // Build in z-order from bottom to top: grinder (swipe overlay + bar +
  // cursor + value text) → mode groups (their button widgets need to
  // intercept taps before the swipe overlay sees them). Toast on top of
  // everything.
  build_grinder(scr);
  build_idle_group(scr);
  build_post_group(scr);

  s_toast_label = lv_label_create(scr);
  lv_obj_set_style_text_color(s_toast_label, kColorAccent, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_toast_label, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_align(s_toast_label, LV_ALIGN_TOP_MID, 0, 30);
  lv_obj_add_flag(s_toast_label, LV_OBJ_FLAG_HIDDEN);

  // Seed grind value from per-preset NVS; clamp in case older firmware stored
  // something outside the new ring's range.
  s_grind.value = std::clamp(presets::last_grind(presets::selected_id()),
                             kGrindMin, kGrindMax);
  // Seed delta bar at the current preset's target so first enter_post lands
  // with the cursor on the user's expected time.
  const auto seed_preset = presets::get(presets::selected_id());
  s_delta.value = std::clamp(static_cast<float>(seed_preset.target_time_s),
                             kDeltaSpec.min, kDeltaSpec.max);

  refresh_preset_label();
  refresh_preset_post_label();
  refresh_grind_value_label();
  refresh_stars();
  refresh_taste_toggles();
  refresh_submit_enabled();
  refresh_suggestion();

  // Force a layout pass so labels have concrete widths before the first
  // refresh reads them for centering — otherwise first-frame positions are
  // off-by-half-label until LVGL gets around to its own layout tick.
  lv_obj_update_layout(scr);
  refresh_grinder();
  start_grind_value_intro_anim();
  // Seed climate tiles immediately — BME280 has been sampling at 1 Hz since
  // its own task started, so a reading is usually ready by the time the UI
  // builds. Without this the strip flashes "--" for up to a second on boot.
  refresh_climate_tiles();

  apply_mode();  // s_mode starts Idle; hides post group

  lv_timer_create(update_climate_strip, 1000, nullptr);

  display::unlock();
}

}  // namespace espressopost::ui
