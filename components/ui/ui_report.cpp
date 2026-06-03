#include "ui.hpp"
#include "ui_bar.hpp"
#include "ui_preset_edit.hpp"
#include "ui_preset_readout.hpp"
#include "ui_presets.hpp"
#include "ui_theme.hpp"
#include "ui_stepper.hpp"

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
// Geometry — the shared layout frame (screen size, the two separators, center
// line, and the shared center-line button geometry) lives in ui_theme.hpp. The
// post-only pill widths + insets below derive from that frame and stay here
// until the post tuning section consolidates them (Phase 2d).
// ---------------------------------------------------------------------------
// Both post-mode pills carry icon + text (✕ Cancel, Submit ›), so they're
// wider than the plain kPostBtnW pills. Each grows away from its anchored
// edge — Cancel rightward from its left inset, Submit leftward from its right
// inset — so the extra width doesn't disturb the anchored edge. Kept as two
// consts so the pills can be sized independently even though they match today.
constexpr int32_t kCancelBtnW        = kPostBtnW + 20;
constexpr int32_t kSubmitBtnW        = kPostBtnW + 20;
// Edge insets for the post-mode pills: kCancelButtonX is the ✕ Cancel pill's
// left inset, kSubmitButtonX the Submit pill's right inset. Decoupled from the
// primary right inset and from each other so the two columns can drift
// independently; seeded to one value so the pair sits symmetric about the
// readout.
constexpr int32_t kCancelButtonX        = kPrimaryBtnRightInset - 10;
constexpr int32_t kSubmitButtonX        = kPrimaryBtnRightInset - 10;

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
// grinder-cap separator and the bar. Tightening this knob pulls the
// value (and via the derived kBrew* offsets, the post BREW TIME value)
// closer to its bar.
constexpr int32_t kGrindValueY           = 108;

// "GRIND VALUE" caption — absolute screen coords (set_pos, not align) so it
// stays parked on the left while the centered big-number to its right grows
// and shrinks with the digit count. Y is picked to vertically center against
// the Montserrat-46 glyph row at kGrindValueY.
constexpr int32_t kGrindCaptionX         = 50;
constexpr int32_t kGrindCaptionY         = 335;

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

// Grind bar y on screen — the bar's placement (its visual width, tick tiers,
// and feel tuning live in ui_bar.hpp). Sized to hug the round-display chord at
// this y. kGrindSpec.y is seeded from here; refresh_suggestion_arrow positions
// the suggestion triangle relative to it.
constexpr int32_t kBarY                  = 384;

// Brew time — value is the user's reported actual brew time in seconds,
// captured in post mode via the (-)/(+) buttons. Range starts at 0 (a
// coffee can't run negative time) and tops out below the 3-digit threshold
// (a 100+ s pull means the basket is choked and the user is dumping it,
// not journaling it). Stored verbatim in ShotRecord.actual_time_s; the
// model and journal derive any delta-vs-target on the fly from the live
// preset target.
constexpr uint8_t kBrewMinS = 0;
constexpr uint8_t kBrewMaxS = 99;

constexpr uint8_t kMaxStars = 5;
// Star-row geometry — hoisted to file scope so on_star_swipe (defined
// before build_post_group) can map indev x → star count via threshold
// crossings. Tuning lives here; visual + tap behavior is in build_star_*
// and the swipe overlay in build_post_group.
constexpr int32_t kStarSize     = 38;
constexpr int32_t kStarGap      = 8;
// Hit-area padding on every side of the star row's swipe overlay.
// Adjacent stars overlap in their notional hit zones — that's fine since
// the swipe handler resolves the single active star from the finger x.
constexpr int32_t kStarExtClick = 10;

// ---------------------------------------------------------------------------
// Accent + mode-specific colors. The base palette (bg, text, muted tiers) and
// the COLOR() hex shim live in ui_theme.hpp. Each accent below is its own const
// so it can be tuned in isolation; the mode-specific ones (POST pill, taste /
// star / stepper, cancel / submit, climate icons) will move into their mode's
// tuning section (Phase 2d). All seeded to amber, so the per-element split is a
// no-op until a value is deliberately changed.
// ---------------------------------------------------------------------------
const lv_color_t kColorPost    = COLOR(0xC88036);  // idle POST pill (border + label)
const lv_color_t kColorTaste   = COLOR(0xC88036);  // Sour/Bitter pills (fill + on-border)
const lv_color_t kColorStar    = COLOR(0xC88036);  // filled rating-star triangles
const lv_color_t kColorToast   = COLOR(0xC88036);  // transient toast label

// Disabled button color
const lv_color_t kColorTasteDisabled = kColorMuted;
// Unlit (disabled) outline color for the star rating row.
const lv_color_t kColorStarDisabled  = kColorMuted;

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

// Post-mode action button palette. Cancel (✕) borrows the climate-tile
// temperature red so it reads as a "stop / undo" semantic; Submit borrows
// the humidity blue when armed (matches the DELTA / suggestion blue tier
// for a tight pull) and falls back to muted gray when the form gates the
// submission. Defined as their own names so the action buttons aren't
// coupled to the climate-icon palette in case either needs to drift.
const lv_color_t kColorCancel         = COLOR(0xE07055);
const lv_color_t kColorSubmitEnabled  = COLOR(0x60A8E0);
const lv_color_t kColorSubmitDisabled = kColorMuted3;

// The Presets-screen palette (Menu/Back/slot/title colors) lives in
// ui_presets.cpp; ui_report reaches for presets_screen::kColorMenu when building
// the Menu pill on the idle center line.

// ---------------------------------------------------------------------------
// UI modes. The grinder bar + cursor + value text stay live across Idle↔Post;
// only the center widgets swap. Presets is a full-panel mode (everything hides)
// reached from Idle via the Menu button — see the section-swap engine below.
// ---------------------------------------------------------------------------
enum class Mode { Idle, Post, Presets, Edit };
constexpr int kModeCount = 4;
Mode s_mode = Mode::Idle;

// One row per mode in the swap registry (s_views, populated once the group
// pointers and per-mode reset hooks further down exist). `group`/`anim` are
// pointer-to-pointer because the widgets they name are filled in at build time;
// `on_enter` reseeds the incoming mode before its fade-in; `on_exit_done` is a
// deferred cleanup run once the outgoing mode is fully hidden. Adding a mode is
// then: add an enum value, build its group, add a row — switch_mode/apply_mode
// never change.
struct ModeView {
  lv_obj_t** group;
  lv_obj_t** anim;
  void (*on_enter)();      // incoming reseed (runs before the swap), or nullptr
  void (*on_exit_done)();  // deferred outgoing cleanup (post-fade), or nullptr
};

// ---------------------------------------------------------------------------
// Grind dial — the scroll-with-momentum bar engine (BarSpec / BarState, draw,
// drag, flick, snap) lives in ui_bar.hpp. kGrindSpec below is the grind dial's
// value-domain instance of it; the grind glue (hooks, overlay forwarder) is
// further down.
// ---------------------------------------------------------------------------
// Grind bar y-band: the bottom area (bar + cursor + big number + captions
// + SUGGESTION) is identical in idle and post, so a press anywhere over the
// GRIND VALUE readout still starts a scrub. visible_half_range × 2 is the
// total value span across the bar's width — smaller half-range → more px
// per tick → finer drag-feel without touching kBarHalfWidth. Tune here
// when the dial feels too twitchy or too coarse.
constexpr BarSpec kGrindSpec = {
  /*min*/                kGrindMin,
  /*max*/                kGrindMax,
  /*step*/               model::kGrindStep,
  /*visible_half_range*/ 0.6f,
  /*half_width*/         kBarHalfWidth,
  /*center_x*/           kCenter,
  /*y*/                  kBarY,
  /*y_band_top*/         298,
  /*y_band_bottom*/      kScreen,
  /*big_every*/          20,
  /*mid_every*/          10,
  /*small_every*/        2,
  /*tick_unit*/          0.05f,
};

// Pixels per grind unit, derived from kGrindSpec so changing the bar density
// automatically keeps refresh_suggestion_arrow's position math correct.
constexpr float kBarPxPerUnit =
    static_cast<float>(kBarHalfWidth) / kGrindSpec.visible_half_range;

// ---------------------------------------------------------------------------
// Widget handles. Grouped by visual role; null until start_report() builds them.
// ---------------------------------------------------------------------------
// Grinder-area extras (always visible across Idle + Post):
// s_grinder_group wraps every always-on grinder widget + the two horizontal
// separators so the whole bottom chrome can hide as one unit when the Presets
// screen takes over the full panel. It stays shown across Idle↔Post (only its
// children's opacity is touched there, and only by the per-section fade).
lv_obj_t* s_grinder_group     = nullptr;
lv_obj_t* s_grinder_overlay   = nullptr;  // transparent full-screen swipe catcher; dispatches to both bars
lv_obj_t* s_static_cursor     = nullptr;  // upward-pointing triangle just below the grind bar
lv_obj_t* s_suggestion_arrow  = nullptr;  // confidence-tinted suggestion triangle over the cursor
lv_obj_t* s_grind_value_label = nullptr;  // big "5.10" above the bar — visible in idle mode
lv_obj_t* s_grind_caption      = nullptr; // "GRIND VALUE" header next to the big number
lv_obj_t* s_suggestion_caption = nullptr; // static "SUGGESTION" header, right of the big value
lv_obj_t* s_suggested_label   = nullptr;  // "x.xx (xx%)" value line under SUGGESTION caption

// The grind bar — spec is wired at start_report(), widget at build_grinder.
// Shown in both modes.
BarState s_grind = {};

// Brew time captured by the post-mode stepper (the shared ui_stepper instance).
// `touched` flips false→true on the first tap (so the value shows "--" until
// then and Submit stays gated); `value` carries the current seconds within
// [kBrewMinS, kBrewMaxS]. min/max/unit/on_change/value_lbl are wired when the
// stepper is built.
StepperState s_brew = {};

// Idle group:
lv_obj_t* s_idle_group          = nullptr;
// Full-screen transparent sub-container holding ONLY the climate tiles — the
// part that fades/slides on a mode swap. Its siblings in s_idle_group (vertical
// dividers, center-line POST/preset buttons) stay put and just show/hide with
// the mode, so the animation never redraws them.
lv_obj_t* s_climate_anim        = nullptr;
lv_obj_t* s_preset_btn          = nullptr;   // tappable preset cycler on the idle center line
lv_obj_t* s_menu_btn            = nullptr;   // idle center-line Menu pill (left), mirrors Post; opens Presets

// PresetReadout (the "PRESET N / dose → yield / time" handle bundle) + its
// builders + apply_readout live in ui_preset_readout.{hpp,cpp} — shared by the
// idle/post readouts here and the Presets-screen grid. s_idle_preset and
// s_post_preset below are the idle (tappable) and post (read-only) instances,
// both pushed by refresh_preset_label().
lv_obj_t* s_post_btn            = nullptr;

// Idle-mode readout (lives inside s_preset_btn) and post-mode read-only
// readout (lives inside s_post_group). The post variant has no button
// chrome since cycling presets mid-form would invalidate the brew-time
// pre-seed (see reset_post_form). QUALITY caption sits next to the star
// row in the middle band.
PresetReadout s_idle_preset     = {};
PresetReadout s_post_preset     = {};
lv_obj_t*     s_quality_caption = nullptr;

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
// Full-screen transparent sub-container holding ONLY the animated post content
// (brew-time block + quality block). The center-line widgets (read-only preset,
// Cancel, Submit) stay direct children of s_post_group and switch instantly.
lv_obj_t* s_post_anim           = nullptr;
// Transparent click-eater, parented to scr and normally hidden. Brought to the
// foreground and shown only for the length of a mode cross-fade so a stray tap
// can't land on either group mid-flight (e.g. Submit on the fading post form).
lv_obj_t* s_swap_block          = nullptr;
lv_obj_t* s_brew_time_caption   = nullptr;  // "BREW TIME" caption at the top of the post screen
// The brew "--"/value readout + (-)/(+) discs are owned by the shared
// ui_stepper instance (s_brew), not tracked here.
lv_obj_t* s_star_icons[kMaxStars] = {};      // custom-drawn star widgets (outline when unlit, filled fan when lit); tap+swipe handled by a shared overlay (see build_post_group)
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

// Popup — one reusable centered card that backs both the transient toast
// (0 buttons, auto-dismiss) and the out-of-band tip (1 dismiss button), and is
// ready for a future confirm dialog (2 buttons + input-blocking scrim). Widgets
// are built once in build_popup() and reconfigured per show_popup() call.
lv_obj_t*     s_popup_scrim       = nullptr;  // full-screen click-eater + dim, shown only when a popup asks for it
lv_obj_t*     s_popup_card        = nullptr;  // centered card container
lv_obj_t*     s_popup_body        = nullptr;  // wrapping body label
lv_obj_t*     s_popup_btn[2]      = {nullptr, nullptr};
lv_obj_t*     s_popup_btn_lbl[2]  = {nullptr, nullptr};
lv_event_cb_t s_popup_btn_cb[2]   = {nullptr, nullptr};  // per-show action, invoked after the card hides
lv_timer_t*   s_popup_timer       = nullptr;  // auto-dismiss timer (0-button popups only)
bool          s_popup_click_outside = true;   // current popup: tapping the scrim dismisses (no action)

// Presets screen (Mode::Presets) — the view (title + 3×3 grid + Back) lives in
// ui_presets.cpp. ui_report only keeps the group handle (set in start_report
// from presets_screen::build) to show/hide it across the mode swap.
lv_obj_t* s_presets_group = nullptr;

// Preset editor (Mode::Edit) — the view lives in ui_preset_edit.cpp; ui_report
// keeps the group handle and the slot id currently being edited (set when a grid
// slot is tapped, consumed by preset_edit::load at the swap midpoint).
lv_obj_t* s_edit_group = nullptr;
uint8_t   s_edit_slot  = 0;

// Records which panels an in-flight section swap is moving between, so the shared
// midpoint hook (panel_swap_mid) knows what to hide / reveal. One swap at a time
// (input is blocked during it), so a single pair is enough.
Mode s_swap_from = Mode::Idle;
Mode s_swap_to   = Mode::Idle;

// Section-swap fade sets — every widget that fades on a full-panel transition
// (Idle↔Presets, Presets↔Edit). The idle set is populated in start_report from
// local handles; the presets / edit sets are owned by their views and pointed at
// via fade_widgets(). The engine just iterates them (skipping any HIDDEN widget).
lv_obj_t* s_idle_fade[16]      = {};
int       s_idle_fade_n        = 0;
lv_obj_t* const* s_presets_fade = nullptr;
int       s_presets_fade_n     = 0;
lv_obj_t* const* s_edit_fade    = nullptr;
int       s_edit_fade_n        = 0;
// No-op anim target: drives the swap's two-phase timing independently of which
// section widgets are visible, so the phase callbacks fire after exactly one
// kModeSwapFadeMs regardless of how many widgets actually faded.
int32_t s_swap_driver = 0;

// ---------------------------------------------------------------------------
// Form / model state.
// ---------------------------------------------------------------------------
model::Suggestion s_current_suggestion = {std::nanf(""), 0};

uint8_t s_stars_value = 0;
// Bitfield of kTasteSour | kTasteBitter — toggled by the two Post pills. Reset
// in reset_post_form. Submitted into ShotRecord.taste_flags verbatim.
uint8_t s_taste_flags = 0;

// Climate snapshot taken when the user enters Post mode (in reset_post_form).
// The shot is logged against the conditions at the moment they started rating,
// not whatever the BME280 reads seconds later at Submit — the strip keeps
// ticking live behind the form, but the record should reflect the brew.
climate::Reading s_post_climate = {};

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
constexpr int32_t kSuggestionArrowTipGap   = 0;

ArrowState s_cursor_arrow_state = {0.0f, COLOR(0xE0E0E0),
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

// Push the current preset into both selected-preset readouts (idle + post).
void refresh_preset_label() {
  const uint8_t slot = presets::selected_id();
  const auto    p    = presets::get(slot);
  apply_readout(s_idle_preset, p, slot);
  apply_readout(s_post_preset, p, slot);
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

// Flip the Sour / Bitter pills against s_taste_flags. The on state reads
// as a solid accent pill with bg-colored text; the off state reads as a
// muted-gray outline + matching text so it sits quietly until tapped.
void refresh_taste_toggles() {
  struct { lv_obj_t* btn; lv_obj_t* lbl; uint8_t mask; } pills[] = {
      {s_sour_btn,   s_sour_label,   storage::kTasteSour},
      {s_bitter_btn, s_bitter_label, storage::kTasteBitter},
  };
  for (auto& pill : pills) {
    if (pill.btn == nullptr) continue;
    const bool on = (s_taste_flags & pill.mask) != 0;
    lv_obj_set_style_bg_opa(pill.btn,
                            on ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(pill.btn, kColorTaste, LV_PART_MAIN);
    lv_obj_set_style_border_color(pill.btn,
                                  on ? kColorTaste : kColorTasteDisabled, LV_PART_MAIN);
    if (pill.lbl != nullptr) {
      lv_obj_set_style_text_color(pill.lbl,
                                  on ? kColorBg : kColorTasteDisabled, LV_PART_MAIN);
    }
  }
}

void refresh_submit_enabled() {
  if (s_submit_btn == nullptr) return;
  // Submit unlocks only after BOTH quality is set AND the user has tapped
  // (-) or (+) at least once. The touched gate prevents a one-tap submit
  // that records the preset's default brew time verbatim — even a pull
  // that landed exactly on target should be an explicit confirmation, not
  // the form's initial state.
  const bool ready  = s_stars_value > 0 && s_brew.touched;
  const lv_color_t color =
      ready ? kColorSubmitEnabled : kColorSubmitDisabled;
  // Outline-only button — toggle the border + label color in lockstep
  // so the disabled state reads as a muted ghost of the armed one. We
  // intentionally do NOT use LV_STATE_DISABLED: LVGL's theme injects a
  // half-opacity override on that state that shadows our color choice.
  // on_submit's own gate (s_stars_value > 0 && s_brew.touched) drops
  // any click that lands while disabled, so we don't need the state
  // to suppress events either.
  lv_obj_set_style_border_color(s_submit_btn, color, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_submit_label, color, LV_PART_MAIN);
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
// Defined with the other anim helpers below. Sequentially swaps two mode groups;
// apply_mode (instant) is used for the initial build, switch_mode animates.
void animate_mode_swap(lv_obj_t* out_group, lv_obj_t* out_content,
                       lv_obj_t* in_group, lv_obj_t* in_content,
                       void (*on_out_done)());

// Full-panel transition engine (every section fades; defined with the other anim
// helpers below). Used for Idle↔Presets and Presets↔Edit. panel_swap_mid runs the
// group hide/show bookkeeping at the swap midpoint (for whichever from→to panels
// the in-flight swap recorded).
void animate_section_swap(lv_obj_t* const* out, int out_n,
                          lv_obj_t* const* in, int in_n, void (*on_mid)());
void panel_swap_mid();

// Replay a short slide-up + fade-in on the center-line preset readout after its
// text is swapped (preset cycle). Defined with the other anim helpers below.
void animate_preset_readout_in(lv_obj_t* root);

// Reset post-form state to the preset's defaults. Shared by the Post view's
// on_enter (reseed before the fade-in) and on_exit_done (deferred clear once
// the post group is fully hidden, so it doesn't visibly blank mid-fade out).
// Grind survives — the user already dialed it for this shot and the bar/cursor
// are still showing that.
void reset_post_form() {
  s_stars_value   = 0;
  s_taste_flags   = 0;
  s_brew.touched  = false;
  // Freeze the climate reading for this shot. on_submit logs this snapshot
  // rather than re-reading climate::latest(), so the record matches what the
  // strip showed when rating began.
  s_post_climate  = climate::latest();
  // Pre-seed the brew time at the preset's target so the first (-)/(+) tap
  // can promote it straight to the user's normal target value without an
  // intervening jump. The "--" display stays until `touched` flips.
  const auto p  = presets::get(presets::selected_id());
  s_brew.value  = std::clamp<uint8_t>(p.target_time_s, kBrewMinS, kBrewMaxS);
  refresh_stars();
  refresh_taste_toggles();
  stepper_refresh(&s_brew);
  refresh_submit_enabled();
}

// Swap registry — indexed by Mode. Post carries reset_post_form as both its
// incoming reseed and its deferred outgoing clear; Idle needs neither.
const ModeView s_views[kModeCount] = {
    /*Idle*/    {&s_idle_group,    &s_climate_anim,  nullptr,         nullptr},
    /*Post*/    {&s_post_group,    &s_post_anim,     reset_post_form, reset_post_form},
    // Presets / Edit use the section-swap engine, not animate_mode_swap — only
    // their `group` is consumed (by apply_mode, to hide it at boot). `anim`/hooks
    // are never read for these rows, so `anim` just mirrors `group`.
    /*Presets*/ {&s_presets_group, &s_presets_group, nullptr,         nullptr},
    /*Edit*/    {&s_edit_group,    &s_edit_group,    nullptr,         nullptr},
};

// Full-panel modes drive the per-section fade engine (vs the Idle↔Post climate
// swap). Their fade set + group bookkeeping are addressed by mode below.
bool is_panel_mode(Mode m) { return m == Mode::Presets || m == Mode::Edit; }

// The section-fade set for a mode (Idle participates as the Presets edge's other
// side). Post never section-swaps, so it returns empty.
lv_obj_t* const* mode_fade(Mode m, int* n) {
  switch (m) {
    case Mode::Idle:    *n = s_idle_fade_n;    return s_idle_fade;
    case Mode::Presets: *n = s_presets_fade_n; return s_presets_fade;
    case Mode::Edit:    *n = s_edit_fade_n;    return s_edit_fade;
    default:            *n = 0;                return nullptr;
  }
}

// Instant mode apply — used for the initial build (no animation). Shows the
// active mode's group, hides the rest.
void apply_mode() {
  for (int i = 0; i < kModeCount; ++i) {
    lv_obj_t* g = *s_views[i].group;
    if (i == static_cast<int>(s_mode)) lv_obj_remove_flag(g, LV_OBJ_FLAG_HIDDEN);
    else                               lv_obj_add_flag(g, LV_OBJ_FLAG_HIDDEN);
  }
  // SUGGESTION block is shown in every mode; let refresh_suggested_label
  // recompute its visibility based on suggestion availability alone.
  refresh_suggested_label();
}

// Animated transition to `target`. Reseeds the incoming mode (on_enter) before
// the fade, then hands the outgoing mode's deferred clear (on_exit_done) to the
// swap engine to run once that group is fully hidden.
void switch_mode(Mode target) {
  if (s_mode == target) return;
  const Mode prev = s_mode;
  s_mode = target;

  // A transition touching a full-panel mode (Presets / Edit) runs the per-section
  // fade engine instead of the climate-band animate_mode_swap. The edges in play
  // are Idle↔Presets and Presets↔Edit; panel_swap_mid hides the outgoing panel
  // and reveals + reseeds the incoming one (grid refresh / editor load) at the
  // midpoint, for whichever from→to this swap records.
  if (is_panel_mode(prev) || is_panel_mode(target)) {
    s_swap_from = prev;
    s_swap_to   = target;
    int out_n = 0, in_n = 0;
    lv_obj_t* const* out = mode_fade(prev, &out_n);
    lv_obj_t* const* in  = mode_fade(target, &in_n);
    animate_section_swap(out, out_n, in, in_n, panel_swap_mid);
    return;
  }

  // Idle↔Post — climate-band swap (unchanged).
  const ModeView& from = s_views[static_cast<int>(prev)];
  const ModeView& to   = s_views[static_cast<int>(target)];
  if (to.on_enter) to.on_enter();
  // SUGGESTION block is shared across modes, so refresh it now.
  refresh_suggested_label();
  animate_mode_swap(*from.group, *from.anim, *to.group, *to.anim,
                    from.on_exit_done);
}

// ---------------------------------------------------------------------------
// Popup — reusable centered card (toast / tip / future confirm dialog).
// ---------------------------------------------------------------------------
// Geometry + colors for the card. Kept local to the popup since nothing else
// references them; the button colors borrow the existing action palette so the
// popup reads as part of the same UI.
constexpr int32_t  kPopupCardW      = 320;  // fixed width; height tracks content
constexpr int32_t  kPopupPad        =  22;
constexpr int32_t  kPopupGap        =  16;  // body→button-row gap
constexpr int32_t  kPopupBtnGap     =  14;  // gap between two buttons
constexpr uint32_t kToastDismissMs  = 3000; // auto-dismiss for the 0-button toast
const lv_color_t kColorPopupCardBg         = COLOR(0x161616);
const lv_color_t kColorPopupBorder         = kColorMuted3;
const lv_color_t kColorPopupBtnNeutral     = kColorText;    // acknowledge / cancel
const lv_color_t kColorPopupBtnDestructive = kColorCancel;  // delete / discard

enum class PopupBtnStyle { Neutral, Destructive };

struct PopupButton {
  const char*   text;
  PopupBtnStyle style;
  lv_event_cb_t on_tap;  // invoked after the card hides; nullptr = dismiss only
};

struct PopupConfig {
  const char* body;
  uint32_t    auto_dismiss_ms;  // 0 = stay until a button is tapped
  bool        scrim;            // block + dim the screen behind the card
  uint8_t     n_buttons;        // 0..2
  PopupButton buttons[2];
  bool        click_outside = true;  // tap the scrim to dismiss with no action (scrimmed popups only)
};

void hide_popup() {
  if (s_popup_timer) { lv_timer_delete(s_popup_timer); s_popup_timer = nullptr; }
  lv_obj_add_flag(s_popup_card, LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(s_popup_scrim, LV_OBJ_FLAG_HIDDEN);
}

void popup_timer_cb(lv_timer_t*) { hide_popup(); }

// Tap on the scrim = tap outside the card. Dismiss with no action when the
// current popup opted in (the default); otherwise the scrim just eats the tap so
// the popup stays modal until a button is pressed.
void popup_scrim_tapped(lv_event_t*) {
  if (s_popup_click_outside) hide_popup();
}

// Both buttons share one trampoline each: capture the per-show action, dismiss
// the card first (so the action can open another screen cleanly), then run it.
void popup_btn_tapped(int idx, lv_event_t* e) {
  const lv_event_cb_t cb = s_popup_btn_cb[idx];
  hide_popup();
  if (cb) cb(e);
}
void popup_btn0_cb(lv_event_t* e) { popup_btn_tapped(0, e); }
void popup_btn1_cb(lv_event_t* e) { popup_btn_tapped(1, e); }

void show_popup(const PopupConfig& cfg) {
  s_popup_click_outside = cfg.click_outside;
  lv_label_set_text(s_popup_body, cfg.body);

  for (int i = 0; i < 2; ++i) {
    if (i < cfg.n_buttons) {
      const PopupButton& b = cfg.buttons[i];
      const lv_color_t c = (b.style == PopupBtnStyle::Destructive)
                               ? kColorPopupBtnDestructive
                               : kColorPopupBtnNeutral;
      lv_label_set_text(s_popup_btn_lbl[i], b.text);
      lv_obj_set_style_border_color(s_popup_btn[i], c, LV_PART_MAIN);
      lv_obj_set_style_text_color(s_popup_btn_lbl[i], c, LV_PART_MAIN);
      s_popup_btn_cb[i] = b.on_tap;
      lv_obj_remove_flag(s_popup_btn[i], LV_OBJ_FLAG_HIDDEN);
    } else {
      s_popup_btn_cb[i] = nullptr;
      lv_obj_add_flag(s_popup_btn[i], LV_OBJ_FLAG_HIDDEN);
    }
  }
  // Collapse the whole button row (the buttons' shared parent) for a 0-button
  // toast so it doesn't leave an empty gap below the body text.
  lv_obj_t* row = lv_obj_get_parent(s_popup_btn[0]);
  if (cfg.n_buttons == 0) lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
  else                    lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);

  if (cfg.scrim) {
    lv_obj_move_foreground(s_popup_scrim);
    lv_obj_remove_flag(s_popup_scrim, LV_OBJ_FLAG_HIDDEN);
  } else {
    lv_obj_add_flag(s_popup_scrim, LV_OBJ_FLAG_HIDDEN);
  }
  lv_obj_move_foreground(s_popup_card);  // always above the scrim (and the UI)
  lv_obj_remove_flag(s_popup_card, LV_OBJ_FLAG_HIDDEN);

  if (s_popup_timer) { lv_timer_delete(s_popup_timer); s_popup_timer = nullptr; }
  if (cfg.auto_dismiss_ms > 0) {
    s_popup_timer = lv_timer_create(popup_timer_cb, cfg.auto_dismiss_ms, nullptr);
  }
}

// Quick status line: a 0-button popup that auto-dismisses after kToastDismissMs,
// but also closes early on a tap outside its card (click_outside defaults true).
// Scrimmed so the tap-outside has a catcher and the message reads as modal for
// its brief life.
void show_toast(const char* text) {
  show_popup(PopupConfig{text, kToastDismissMs, /*scrim=*/true, 0, {}});
}

// Build the popup widget tree once (called from start_report, topmost in
// z-order). show_popup() only flips visibility / text / colors afterward.
void build_popup(lv_obj_t* scr) {
  // Scrim: full-screen click-eater + dim behind the card, and the catcher for
  // tap-outside-to-dismiss. Shown for popups that ask for it (toast + confirm
  // dialogs); the non-scrim tip leaves it hidden so the UI behind stays live.
  s_popup_scrim = lv_obj_create(scr);
  lv_obj_set_size(s_popup_scrim, kScreen, kScreen);
  lv_obj_set_pos(s_popup_scrim, 0, 0);
  lv_obj_set_style_bg_color(s_popup_scrim, kColorBg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_popup_scrim, LV_OPA_50, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_popup_scrim, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_popup_scrim, 0, LV_PART_MAIN);
  lv_obj_set_style_radius(s_popup_scrim, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_popup_scrim, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_popup_scrim, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_popup_scrim, LV_OBJ_FLAG_HIDDEN);
  // A scrim tap is a tap outside the card — dismiss when the popup opted in.
  lv_obj_add_event_cb(s_popup_scrim, popup_scrim_tapped, LV_EVENT_CLICKED, nullptr);

  // Card: centered, fixed width, height tracks content. Vertical flex stacks
  // the wrapping body label over the button row.
  s_popup_card = lv_obj_create(scr);
  lv_obj_set_width(s_popup_card, kPopupCardW);
  lv_obj_set_height(s_popup_card, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_color(s_popup_card, kColorPopupCardBg, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_popup_card, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_radius(s_popup_card, kPopupPad, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_popup_card, kColorPopupBorder, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_popup_card, 2, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_popup_card, kPopupPad, LV_PART_MAIN);
  lv_obj_set_style_pad_row(s_popup_card, kPopupGap, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_popup_card, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(s_popup_card, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(s_popup_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(s_popup_card, LV_OBJ_FLAG_SCROLLABLE);
  // Eat taps that land on the card (but off its buttons) so they don't fall
  // through to the scrim below and dismiss a click-outside popup from inside.
  lv_obj_add_flag(s_popup_card, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(s_popup_card, LV_ALIGN_CENTER, 0, 0);
  lv_obj_add_flag(s_popup_card, LV_OBJ_FLAG_HIDDEN);

  s_popup_body = lv_label_create(s_popup_card);
  lv_obj_set_width(s_popup_body, kPopupCardW - 2 * kPopupPad);
  lv_label_set_long_mode(s_popup_body, LV_LABEL_LONG_WRAP);
  // Amber body — carries over the old toast's accent and reads as "attention"
  // for the out-of-band tip.
  lv_obj_set_style_text_color(s_popup_body, kColorToast, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_popup_body, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_align(s_popup_body, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

  // Button row — up to two outline pills (same idiom as the Cancel/Submit
  // pills); each hides when unused, and the whole row hides for 0-button toasts.
  lv_obj_t* row = lv_obj_create(s_popup_card);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(row, kPopupBtnGap, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(row, 0, LV_PART_MAIN);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  const lv_event_cb_t btn_cbs[2] = {popup_btn0_cb, popup_btn1_cb};
  for (int i = 0; i < 2; ++i) {
    lv_obj_t* b = lv_button_create(row);
    lv_obj_set_size(b, kPostBtnW, kPostBtnH);
    lv_obj_set_style_radius(b, kPostBtnH / 2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, kPostBtnStroke, LV_PART_MAIN);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_ext_click_area(b, kPostBtnExtClick);
    lv_obj_add_event_cb(b, btn_cbs[i], LV_EVENT_CLICKED, nullptr);
    lv_obj_t* lbl = lv_label_create(b);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_obj_center(lbl);
    s_popup_btn[i]     = b;
    s_popup_btn_lbl[i] = lbl;
    lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
  }
}

// Out-of-band tip: a 1-button ("Got it") informational popup shown after a
// saved shot whose actual time defied a confident prediction. Direction-aware
// copy with the actual-vs-expected numbers; the cause list stays a menu (we
// don't claim which one), matching the diagnostic, non-prescriptive intent.
void show_out_of_band_tip(const model::ShotAssessment& a, uint8_t actual_s) {
  const int pred = static_cast<int>(std::lround(a.predicted_time_s));
  char body[160];
  if (a.verdict == model::ShotVerdict::RanLong) {
    std::snprintf(body, sizeof(body),
                  "Pull ran long — %us vs ~%ds expected.\n"
                  "Check dose, grind, or backflush.",
                  static_cast<unsigned>(actual_s), pred);
  } else {  // RanShort
    std::snprintf(body, sizeof(body),
                  "Pull ran fast — %us vs ~%ds expected.\n"
                  "Possible channeling or coarse grind.",
                  static_cast<unsigned>(actual_s), pred);
  }
  PopupConfig cfg{};
  cfg.body            = body;
  cfg.auto_dismiss_ms = 0;      // stays until acknowledged
  cfg.scrim           = false;  // non-modal; the UI behind stays live
  cfg.n_buttons       = 1;
  cfg.buttons[0]      = PopupButton{"Got it", PopupBtnStyle::Neutral, nullptr};
  show_popup(cfg);
}

// ---------------------------------------------------------------------------
// Event handlers.
// ---------------------------------------------------------------------------
void on_preset_tap(lv_event_t*) {
  if (s_mode != Mode::Idle) return;  // belt-and-suspenders: hidden in post mode
  const auto new_id = presets::cycle_selected();
  s_grind.value = std::clamp(presets::last_grind(new_id), kGrindMin, kGrindMax);
  refresh_preset_label();
  animate_preset_readout_in(s_idle_preset.root);  // slide+fade the new values in
  refresh_grind_value_label();
  refresh_suggestion();
  refresh_grinder();
}

void on_post_tap(lv_event_t*) {
  switch_mode(Mode::Post);
}

void on_cancel_tap(lv_event_t*) {
  switch_mode(Mode::Idle);
}

void on_menu_tap(lv_event_t*) {
  if (s_mode != Mode::Idle) return;  // Menu only lives on the idle center line
  switch_mode(Mode::Presets);
}

void on_back_tap(lv_event_t*) {
  switch_mode(Mode::Idle);
}

// Tapping a Presets grid slot opens its editor. The slot's 0-based id rides in
// the tapped widget's user_data (stored by ui_presets); stash it for
// preset_edit::load (run at the swap midpoint) and switch to the Edit panel.
void on_slot_tap(lv_event_t* e) {
  if (s_mode != Mode::Presets) return;
  s_edit_slot = static_cast<uint8_t>(reinterpret_cast<intptr_t>(
      lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e)))));
  switch_mode(Mode::Edit);
}

// Editor Cancel — back to the grid, no write.
void on_edit_cancel_tap(lv_event_t*) {
  switch_mode(Mode::Presets);
}

// Editor Save — compose the edited preset and persist it (creating the slot if
// it was empty), then return to the grid. gather() returns false if the form
// isn't complete, but Save is gated to only fire when it is, so that's a no-op
// guard.
void on_edit_save_tap(lv_event_t*) {
  presets::Preset p;
  if (preset_edit::gather(&p)) {
    presets::set(s_edit_slot, p);
    // Re-push the center-line readout so a new accent/values show at once if the
    // edited slot is the selected one (no-op for the others — it re-reads the
    // selected preset).
    refresh_preset_label();
  }
  switch_mode(Mode::Presets);
}

// Confirm button of the delete popup: drop the slot's blob AND its saved posts,
// then return to the grid. Purging the shots first keeps the deleted preset's
// history from bleeding into a future preset that reuses the slot index; the
// model is refit so its in-memory fits match the now-smaller log. clear()
// auto-advances the selection if the deleted slot was selected, so re-push the
// center-line readout (it's behind the panel now, but correct when idle
// reappears).
void confirm_delete_preset(lv_event_t*) {
  storage::purge_preset_shots(s_edit_slot);
  presets::clear(s_edit_slot);
  model::refit();
  refresh_preset_label();
  switch_mode(Mode::Presets);
}

// Editor trash — ask before deleting. The device always needs at least one
// preset, so deleting the only remaining one is refused with a brief note rather
// than a confirm. The confirm warns when the slot has saved posts, since delete
// takes those with it. Both use the shared scrimmed popup.
void on_edit_delete_tap(lv_event_t*) {
  if (presets::count() <= 1) {
    PopupConfig info{};
    info.body       = "Keep at least one preset.";
    info.scrim      = true;
    info.n_buttons  = 1;
    info.buttons[0] = PopupButton{"OK", PopupBtnStyle::Neutral, nullptr};
    show_popup(info);
    return;
  }
  const uint32_t posts = storage::shot_count_for_preset(s_edit_slot);
  char body[96];
  const int len = std::snprintf(body, sizeof(body), "Delete PRESET %u?",
                                static_cast<unsigned>(s_edit_slot + 1));
  if (posts > 0 && len > 0 && static_cast<size_t>(len) < sizeof(body)) {
    std::snprintf(body + len, sizeof(body) - len,
                  "\nThis will delete %u saved %s!",
                  static_cast<unsigned>(posts), posts == 1 ? "post" : "posts");
  }
  PopupConfig cfg{};
  cfg.body       = body;
  cfg.scrim      = true;
  cfg.n_buttons  = 2;
  cfg.buttons[0] = PopupButton{"Cancel", PopupBtnStyle::Neutral,     nullptr};
  cfg.buttons[1] = PopupButton{"Delete", PopupBtnStyle::Destructive, confirm_delete_preset};
  show_popup(cfg);
}

// Hit-area-enter swipe handler for the star row. Maps the indev's current
// x to a star count via threshold crossings — one threshold at each star's
// hit-area left edge (visible left edge minus kStarExtClick). Triggered
// on both PRESSED and PRESSING so a quick tap and a drag both work; once
// pressed, LVGL keeps dispatching PRESSING events with the live indev
// point even when the finger leaves the overlay's bounds, so swiping past
// star 0's left edge clears the rating to 0. The prior toggle-on-re-tap
// behavior is intentionally gone — to clear, swipe left of the row.
void on_star_swipe(lv_event_t* e) {
  auto* overlay = static_cast<lv_obj_t*>(lv_event_get_target(e));
  lv_indev_t* indev = lv_indev_active();
  if (indev == nullptr) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);

  lv_area_t coords;
  lv_obj_get_coords(overlay, &coords);
  // overlay.x1 sits kStarExtClick px before star 0's visible left edge.
  const int32_t row_left = coords.x1 + kStarExtClick;

  uint8_t new_value = 0;
  for (uint8_t i = 0; i < kMaxStars; ++i) {
    const int32_t ext_left =
        row_left + i * (kStarSize + kStarGap) - kStarExtClick;
    if (p.x >= ext_left) ++new_value;
  }
  if (new_value == s_stars_value) return;
  s_stars_value = new_value;
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

// The brew-time (-)/(+)/value-tap handlers now live in ui_stepper; the
// stepper fires s_brew.on_change (wired to refresh_submit_enabled) after each
// tap, so Submit re-gates without any post-local handler.

void on_submit(lv_event_t*) {
  if (!(s_stars_value > 0 && s_brew.touched)) return;  // belt-and-suspenders

  const climate::Reading& r = s_post_climate;
  storage::ShotRecord rec = {};
  rec.preset_id       = presets::selected_id();
  rec.actual_time_s   = s_brew.value;
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

  // Judge this shot against the CURRENT fit — must happen BEFORE refit() folds
  // it in, so it's measured against the model that produced the suggestion the
  // user saw. Conservative + confidence-gated inside model::assess_shot.
  const model::ShotAssessment assessment = model::assess_shot(rec);

  switch_mode(Mode::Idle);

  // Out-of-band shots get the diagnostic tip (stays until acknowledged); normal
  // shots get the usual transient "Saved #N" toast. Shown after the mode swap
  // so the popup card foregrounds above the swap block.
  if (assessment.verdict == model::ShotVerdict::InBand) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "Post #%u saved",
                  static_cast<unsigned>(storage::shot_count()));
    show_toast(buf);
  } else {
    show_out_of_band_tip(assessment, rec.actual_time_s);
  }

  // New data point — refit and refresh so the arrow reflects what we just
  // learned the next climate tick (cheap on our data volumes; inline keeps
  // UI deterministic vs deferring to a background task).
  model::refit();
  refresh_suggestion();
  refresh_grinder();
}

// ---------------------------------------------------------------------------
// Grind bar glue — wires the generic bar engine (ui_bar.hpp) to grind: the
// overlay forwarder feeds touches to bar_dispatch_event, and the hooks persist
// + refresh on change/settle.
// ---------------------------------------------------------------------------
// Overlay forwarder. Only the grind bar uses the swipe overlay now — the
// brew time captured by the post-mode (-)/(+) buttons doesn't touch this
// path.
void on_grind_bar_event(lv_event_t* e) {
  bar_dispatch_event(e, &s_grind);
}

// Grind bar hooks invoked by bar_dispatch_event / bar_momentum_tick when
// the value moves, the drag/glide settles, or the bar is touched for the
// first time in a form session.
void grind_on_change(BarState*) {
  refresh_grinder();
  refresh_grind_value_label();
}
void grind_on_settle(BarState*) {
  presets::set_last_grind(presets::selected_id(), s_grind.value);
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
// Tile container geometry. kClimateAreaHeight is the fixed vertical span
// the icon / label / value rows are designed against (their offsets —
// kTileIconY, kTileLabelY, kTileValueY below — are relative to the
// container top). Keeping it independent of kClimateSeparatorY means
// moving the separator only translates the container up or down,
// leaving the internal row positions intact. kClimateTopY can go
// negative when the container extends above the round-display chord; the
// chord clips the unused top band, no further math needed.
constexpr int32_t kClimateAreaHeight = 210;
constexpr int32_t kClimateTopY       =
    kClimateSeparatorY - kClimateAreaHeight;
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

// Whole-group opacity — opa on LV_PART_MAIN cascades to the subtree, so one
// anim fades a mode group and all its children together. The tiles/labels
// don't overlap, so plain opa (no opa_layered layer buffer) blends cleanly
// and skips a full-screen render layer per frame on the embedded target.
void opa_exec(void* var, int32_t v) {
  lv_obj_set_style_opa(static_cast<lv_obj_t*>(var),
                       static_cast<lv_opa_t>(v), LV_PART_MAIN);
}

// Center-line preset readout flip — mirrors the climate tile's slide+fade but on
// the whole readout block at once (it's a single aligned container, so translate
// is purely visual and opa cascades to the labels + arrow). Called after the
// text is swapped on a preset cycle: snap the block kPresetFlipSlideY px down and
// transparent, then ease it back up to its rest pose at full opacity.
constexpr int32_t  kPresetFlipSlideY = 20;
constexpr uint32_t kPresetFlipMs     = 220;
void animate_preset_readout_in(lv_obj_t* root) {
  if (root == nullptr) return;
  lv_obj_set_style_translate_y(root, kPresetFlipSlideY, LV_PART_MAIN);
  lv_obj_set_style_opa(root, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_anim_t slide;
  lv_anim_init(&slide);
  lv_anim_set_var(&slide, root);
  lv_anim_set_exec_cb(&slide, translate_y_exec);
  lv_anim_set_values(&slide, kPresetFlipSlideY, 0);
  lv_anim_set_duration(&slide, kPresetFlipMs);
  lv_anim_set_path_cb(&slide, lv_anim_path_ease_out);
  lv_anim_start(&slide);
  lv_anim_t fade;
  lv_anim_init(&fade);
  lv_anim_set_var(&fade, root);
  lv_anim_set_exec_cb(&fade, opa_exec);
  lv_anim_set_values(&fade, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_duration(&fade, kPresetFlipMs);
  lv_anim_set_path_cb(&fade, lv_anim_path_ease_out);
  lv_anim_start(&fade);
}

// ---------------------------------------------------------------------------
// Mode swap — two sequential phases, fade only (no positioning). Phase A fades
// the outgoing content out; at the midpoint the mode groups switch (so the
// static center-line buttons + separators swap in one instant frame, never
// redrawn mid-anim); phase B fades the incoming content in. Only the content
// sub-containers (s_climate_anim / s_post_anim) animate — never the center line,
// the dividers, or the shared bottom band (all outside those sub-containers).
// One phase animates at a time, so at most one content tree redraws per frame.
// ---------------------------------------------------------------------------
constexpr uint32_t kModeSwapFadeMs = 175;  // each of the two phases

// Single in-flight swap at a time (input is blocked for its duration), so one
// file-static context is enough to carry the phase-boundary state.
struct ModeSwapCtx {
  lv_obj_t* out_group;
  lv_obj_t* out_content;
  lv_obj_t* in_group;
  lv_obj_t* in_content;
  void (*on_out_done)();  // deferred cleanup once the outgoing group is hidden
};
ModeSwapCtx s_mode_swap = {};

// Phase B finished — incoming content rests at full opacity / y=0. Drop the
// input block; the swap is done.
void mode_swap_phase_b_done(lv_anim_t* /*a*/) {
  lv_obj_add_flag(s_swap_block, LV_OBJ_FLAG_HIDDEN);
}

// Midpoint — outgoing content has faded up and away. Hide its whole group so
// the center-line buttons + dividers swap instantly, clear the content's
// transient pose for its next entrance, run the deferred post-form reset
// (leaving post only), then reveal the incoming group and ease its content
// down into place.
void mode_swap_phase_a_done(lv_anim_t* /*a*/) {
  lv_obj_add_flag(s_mode_swap.out_group, LV_OBJ_FLAG_HIDDEN);
  lv_obj_set_style_opa(s_mode_swap.out_content, LV_OPA_COVER, LV_PART_MAIN);
  if (s_mode_swap.on_out_done) s_mode_swap.on_out_done();

  // Incoming group's center-line + separators appear now (instant); its content
  // starts transparent, then fades up to full opacity.
  lv_obj_set_style_opa(s_mode_swap.in_content, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_remove_flag(s_mode_swap.in_group, LV_OBJ_FLAG_HIDDEN);

  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_duration(&a, kModeSwapFadeMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
  lv_anim_set_var(&a, s_mode_swap.in_content);
  lv_anim_set_exec_cb(&a, opa_exec);
  lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
  lv_anim_set_ready_cb(&a, mode_swap_phase_b_done);
  lv_anim_start(&a);
}

void animate_mode_swap(lv_obj_t* out_group, lv_obj_t* out_content,
                       lv_obj_t* in_group, lv_obj_t* in_content,
                       void (*on_out_done)()) {
  s_mode_swap = {out_group, out_content, in_group, in_content, on_out_done};

  // Foreground click-eater swallows taps until the swap finalizes.
  lv_obj_move_foreground(s_swap_block);
  lv_obj_remove_flag(s_swap_block, LV_OBJ_FLAG_HIDDEN);

  // Phase A: fade the outgoing content out. Its group (center-line + dividers)
  // stays put until phase A completes, then swaps in one frame.
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_duration(&a, kModeSwapFadeMs);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
  lv_anim_set_var(&a, out_content);
  lv_anim_set_exec_cb(&a, opa_exec);
  lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
  lv_anim_set_ready_cb(&a, mode_swap_phase_a_done);
  lv_anim_start(&a);
}

// ---------------------------------------------------------------------------
// Section swap — the full-panel Idle↔Presets transition. Unlike animate_mode_
// swap (which fades ONE content container and keeps the shared chrome), Presets
// replaces the whole screen, so every visible section fades. To keep each frame's
// redraw confined to the widgets themselves and never the empty gaps between
// them, each section is its own opa anim rather than one full-screen container.
// Same two-phase shape as the mode swap: fade the outgoing set out, do the
// group hide/show at the midpoint, fade the incoming set in. A no-op "driver"
// anim owns the phase timing so completion doesn't depend on any one widget.
// ---------------------------------------------------------------------------

// Set a uniform opacity on a section set, skipping HIDDEN widgets so their
// (possibly-off) visibility + opacity are left untouched.
void set_section_opa(lv_obj_t* const* widgets, int n, int32_t opa) {
  for (int i = 0; i < n; ++i) {
    lv_obj_t* w = widgets[i];
    if (w == nullptr || lv_obj_has_flag(w, LV_OBJ_FLAG_HIDDEN)) continue;
    lv_obj_set_style_opa(w, opa, LV_PART_MAIN);
  }
}

// Fade a section set from→to in parallel, one confined opa anim per non-HIDDEN
// widget. No ready_cb here — the driver anim (below) owns phase timing.
void fade_section(lv_obj_t* const* widgets, int n, int32_t from_opa,
                  int32_t to_opa, lv_anim_path_cb_t path) {
  for (int i = 0; i < n; ++i) {
    lv_obj_t* w = widgets[i];
    if (w == nullptr || lv_obj_has_flag(w, LV_OBJ_FLAG_HIDDEN)) continue;
    lv_obj_set_style_opa(w, from_opa, LV_PART_MAIN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_duration(&a, kModeSwapFadeMs);
    lv_anim_set_path_cb(&a, path);
    lv_anim_set_var(&a, w);
    lv_anim_set_exec_cb(&a, opa_exec);
    lv_anim_set_values(&a, from_opa, to_opa);
    lv_anim_start(&a);
  }
}

void swap_driver_exec(void* var, int32_t v) {
  *static_cast<int32_t*>(var) = v;  // no visible effect; just advances the clock
}

void start_swap_driver(lv_anim_ready_cb_t ready) {
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_duration(&a, kModeSwapFadeMs);
  lv_anim_set_var(&a, &s_swap_driver);
  lv_anim_set_exec_cb(&a, swap_driver_exec);
  lv_anim_set_values(&a, 0, 1);
  lv_anim_set_ready_cb(&a, ready);
  lv_anim_start(&a);
}

// In-flight section swap: which set fades in at phase B, and the midpoint hook.
// Single swap at a time (input blocked), so one static ctx is enough.
struct SectionSwapCtx {
  lv_obj_t* const* in;
  int              in_n;
  void (*on_mid)();
};
SectionSwapCtx s_sec_swap = {};

void section_swap_phase_b_done(lv_anim_t* /*a*/) {
  lv_obj_add_flag(s_swap_block, LV_OBJ_FLAG_HIDDEN);
}

void section_swap_phase_a_done(lv_anim_t* /*a*/) {
  // Outgoing set has faded away. Hide its groups / reveal the incoming group
  // (on_mid), then fade the incoming set up.
  if (s_sec_swap.on_mid) s_sec_swap.on_mid();
  fade_section(s_sec_swap.in, s_sec_swap.in_n, LV_OPA_TRANSP, LV_OPA_COVER,
               lv_anim_path_ease_out);
  start_swap_driver(section_swap_phase_b_done);
}

void animate_section_swap(lv_obj_t* const* out, int out_n,
                          lv_obj_t* const* in, int in_n, void (*on_mid)()) {
  s_sec_swap = {in, in_n, on_mid};

  // Foreground click-eater swallows taps for the swap's duration.
  lv_obj_move_foreground(s_swap_block);
  lv_obj_remove_flag(s_swap_block, LV_OBJ_FLAG_HIDDEN);

  fade_section(out, out_n, LV_OPA_COVER, LV_OPA_TRANSP, lv_anim_path_ease_in);
  start_swap_driver(section_swap_phase_a_done);
}

// Hide a panel at the swap midpoint: drop its group(s) in one frame and restore
// its faded children to full opacity while hidden, ready for the next show. Idle
// owns two groups (idle chrome + the always-on grinder band).
void hide_panel(Mode m) {
  int n = 0;
  lv_obj_t* const* f = mode_fade(m, &n);
  set_section_opa(f, n, LV_OPA_COVER);
  switch (m) {
    case Mode::Idle:
      lv_obj_add_flag(s_idle_group, LV_OBJ_FLAG_HIDDEN);
      lv_obj_add_flag(s_grinder_group, LV_OBJ_FLAG_HIDDEN);
      break;
    case Mode::Presets: lv_obj_add_flag(s_presets_group, LV_OBJ_FLAG_HIDDEN); break;
    case Mode::Edit:    lv_obj_add_flag(s_edit_group, LV_OBJ_FLAG_HIDDEN); break;
    default: break;
  }
}

// Reveal a panel at the swap midpoint: reseed it (grid refresh / editor load /
// SUGGESTION re-assert for idle) then unhide its group(s). Per-widget HIDDEN
// flags inside the panel are untouched, so e.g. a hidden suggestion arrow stays
// hidden when idle reappears.
void show_panel(Mode m) {
  switch (m) {
    case Mode::Idle:
      refresh_suggested_label();
      lv_obj_remove_flag(s_idle_group, LV_OBJ_FLAG_HIDDEN);
      lv_obj_remove_flag(s_grinder_group, LV_OBJ_FLAG_HIDDEN);
      break;
    case Mode::Presets:
      presets_screen::refresh();
      lv_obj_remove_flag(s_presets_group, LV_OBJ_FLAG_HIDDEN);
      break;
    case Mode::Edit:
      preset_edit::load(s_edit_slot);
      lv_obj_remove_flag(s_edit_group, LV_OBJ_FLAG_HIDDEN);
      break;
    default: break;
  }
}

// Shared midpoint hook: the outgoing panel has faded out — hide it and reveal the
// incoming one, for whichever from→to switch_mode recorded on this swap.
void panel_swap_mid() {
  hide_panel(s_swap_from);
  show_panel(s_swap_to);
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
  // The climate strip + suggestion live under the idle/grinder groups, both
  // hidden while the Presets screen owns the panel — skip the refresh (and its
  // redraws) until we're back on a mode that shows them.
  if (s_mode == Mode::Presets) return;
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
// kStarSize / kStarExtClick live at file scope above (near kMaxStars) so
// on_star_swipe can use them. kStarInnerRatio stays here because it's
// only touched by build_star_polyline.
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
    tri.color = kColorStar;
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
    line.color       = kColorStarDisabled;
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
void build_grinder(lv_obj_t* scr) {
  // Wire the grind bar state. Spec is constexpr; widget+value+hooks attach
  // here. Brew time has no bar in post mode anymore — it's captured by the
  // (-)/(+) buttons built in build_post_group, which dispatch directly via
  // on_brew_minus_tap / on_brew_plus_tap.
  s_grind.spec      = &kGrindSpec;
  s_grind.on_change = grind_on_change;
  s_grind.on_settle = grind_on_settle;
  s_grind.on_touched = nullptr;  // grind doesn't gate any UI on "touched"

  // Full-screen transparent container holding the whole grinder chrome (swipe
  // overlay, bar, cursor, suggestion arrow, value/captions) + the two horizontal
  // separators (parented in build_idle_group). Created before s_idle_group so it
  // paints underneath the climate tiles + buttons; hidden as a unit when the
  // Presets screen owns the full panel.
  s_grinder_group = lv_obj_create(scr);
  lv_obj_set_size(s_grinder_group, kScreen, kScreen);
  lv_obj_set_pos(s_grinder_group, 0, 0);
  lv_obj_set_style_bg_opa(s_grinder_group, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_grinder_group, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_grinder_group, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_grinder_group, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s_grinder_group, LV_OBJ_FLAG_CLICKABLE);

  // Transparent full-screen overlay catches horizontal swipes for the grind
  // bar. on_grind_bar_event hit-tests against kGrindSpec's y_band and
  // ignores events that don't land inside it. Sits BEHIND the climate tiles
  // + Post button + Post-mode buttons so those widgets take their own taps
  // first.
  s_grinder_overlay = lv_obj_create(s_grinder_group);
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
  }

  // Tick bar — custom-drawn widget centered on (kCenter, kBarY). On every
  // drag, LVGL invalidates only this widget's bounds.
  s_grind.widget = make_bar_widget(s_grinder_group, &s_grind);

  // Upward cursor — fixed at bar center, tip points UP at the bar from the
  // row below. Widget bounds (kCursorWidget) are larger than the triangle,
  // so we offset by half that slack to land the tip kCursorTipGap below the
  // big-tick extent of the bar.
  s_cursor_arrow_state.color = kColorText;
  s_static_cursor = make_arrow(s_grinder_group, &s_cursor_arrow_state, kCursorWidget);
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
  s_suggestion_arrow = make_arrow(s_grinder_group, &s_suggestion_arrow_state, kCursorWidget);
  lv_obj_add_flag(s_suggestion_arrow, LV_OBJ_FLAG_HIDDEN);

  // Current value as big text above the bar, visible in BOTH modes. Suggested
  // line tucks between this and the bar (further down).
  s_grind_value_label = lv_label_create(s_grinder_group);
  lv_obj_set_style_text_color(s_grind_value_label, kColorText, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_grind_value_label, &lv_font_montserrat_46,
                             LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_grind_value_label, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_align(s_grind_value_label, LV_ALIGN_CENTER, 0, kGrindValueY);
  lv_obj_clear_flag(s_grind_value_label, LV_OBJ_FLAG_CLICKABLE);

  // Static caption to the left of the value, same muted-gray + Montserrat 14
  // treatment as the climate section captions so the eye reads it as a header
  // for the big number rather than another live readout.
  s_grind_caption = lv_label_create(s_grinder_group);
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
  s_suggestion_caption = lv_label_create(s_grinder_group);
  lv_obj_set_style_text_color(s_suggestion_caption, kColorLabel, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_suggestion_caption, &lv_font_montserrat_14,
                             LV_PART_MAIN);
  lv_label_set_text(s_suggestion_caption, "SUGGESTION");
  lv_obj_set_pos(s_suggestion_caption, kSuggestionBlockX, kSuggestionCaptionY);
  lv_obj_set_width(s_suggestion_caption, kSuggestionBlockW);
  lv_obj_set_style_text_align(s_suggestion_caption, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_add_flag(s_suggestion_caption, LV_OBJ_FLAG_HIDDEN);

  s_suggested_label = lv_label_create(s_grinder_group);
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
  lv_obj_set_size(t.container, tile_w, kClimateAreaHeight);
  lv_obj_set_pos(t.container, left_edge, kClimateTopY);
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
  // The two vertical column dividers are idle-only — parented to s_idle_group
  // so they hide with the climate strip when the top area swaps to post UI.
  // The horizontal cap at kClimateSeparatorY is parented to `scr` (further
  // below, next to the grinder separator) so it stays visible across both
  // modes — in idle it caps the climate strip; in post it doubles as the
  // top boundary of the Quality section.
  make_separator(s_idle_group, kColLeftEdge1 - kSeparatorThickness / 2, kSeparatorInset,
                 kSeparatorThickness, kClimateSeparatorY - 2 * kSeparatorInset);
  make_separator(s_idle_group, kColLeftEdge2 - kSeparatorThickness / 2, kSeparatorInset,
                 kSeparatorThickness, kClimateSeparatorY - 2 * kSeparatorInset);

  // Transparent sub-container that holds ONLY the tiles, so the mode-swap fade
  // touches neither the dividers nor the center-line buttons. Sized to just the
  // climate strip (top edge at 0, bottom at the climate separator) rather than
  // full-screen, so fading its opa invalidates only the top band — not the
  // whole 466×466 panel. At (0,0), so tile coords match parenting on the group.
  s_climate_anim = lv_obj_create(s_idle_group);
  lv_obj_set_size(s_climate_anim, kScreen, kClimateSeparatorY);
  lv_obj_set_pos(s_climate_anim, 0, 0);
  lv_obj_set_style_bg_opa(s_climate_anim, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_climate_anim, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_climate_anim, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_climate_anim, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s_climate_anim, LV_OBJ_FLAG_CLICKABLE);

  build_climate_tile(s_climate_anim, 0, kColLeftEdge0,  kColLeftEdge1,
                     kIconGauge,  kColorIconPressure, "PRESSURE",
                     on_pressure_tap);
  build_climate_tile(s_climate_anim, 1, kColLeftEdge1,  kColLeftEdge2,
                     kIconThermo, kColorIconTemp,    "TEMPERATURE",
                     on_temp_tap);
  build_climate_tile(s_climate_anim, 2, kColLeftEdge2,  kColRightEdge2,
                     kIconDrop,   kColorIconHumidity,   "HUMIDITY",
                     on_humidity_tap);

  // (Grind value sits above the bar; it's created in build_grinder as an
  // always-visible widget so it survives the idle→post toggle.)

  // Post button — opens the post-mode form. Lives in s_idle_group so it hides
  // when entering post (post mode replaces the center line with ✕ / preset /
  // Submit).
  // Width + inset mirror the post-mode Submit pill (text centered for now; an
  // icon will join it later) so Post and Submit share the same right-edge
  // footprint across the mode swap.
  s_post_btn = lv_button_create(s_idle_group);
  lv_obj_set_size(s_post_btn, kSubmitBtnW, kPostBtnH);
  lv_obj_set_style_radius(s_post_btn, kPostBtnH / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_post_btn, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_post_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_post_btn, kColorPost, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_post_btn, kPostBtnStroke, LV_PART_MAIN);
  lv_obj_set_style_border_opa(s_post_btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(s_post_btn, LV_ALIGN_RIGHT_MID, -kSubmitButtonX,
               kCenterLineOffsetY);
  lv_obj_set_ext_click_area(s_post_btn, kPostBtnExtClick);
  lv_obj_add_event_cb(s_post_btn, on_post_tap, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* post_lbl = lv_label_create(s_post_btn);
  lv_obj_set_style_text_color(post_lbl, kColorPost, LV_PART_MAIN);
  lv_obj_set_style_text_font(post_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(post_lbl, "Post");
  lv_obj_center(post_lbl);

  // Horizontal separators capping the climate strip (top) and the grinder
  // area (bottom). Parented to s_grinder_group so they stay visible across
  // Idle↔Post (the climate cap doubles as the Quality-section top boundary
  // in post mode, the grinder cap frames the bottom area) but hide as a unit
  // with the rest of the bottom chrome when the Presets screen takes over.
  make_separator(s_grinder_group, kSeparatorInset,
                 kClimateSeparatorY - kSeparatorThickness / 2,
                 kScreen - 2 * kSeparatorInset, kSeparatorThickness);
  make_separator(s_grinder_group, kSeparatorInset,
                 kGrinderSeparatorY - kSeparatorThickness / 2,
                 kScreen - 2 * kSeparatorInset, kSeparatorThickness);

  // Preset selector — tap-to-cycle two-line label centered on the center
  // line (POST btn lives to the right; preset is the dominant readout).
  // Lives in s_idle_group so it hides in post mode, where the post readout
  // takes its place at the same anchor (read-only — cycling presets
  // mid-form would invalidate the brew-time pre-seed).
  s_preset_btn = lv_button_create(s_idle_group);
  lv_obj_set_style_bg_opa(s_preset_btn, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_preset_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_preset_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_preset_btn, 6, LV_PART_MAIN);
  lv_obj_set_size(s_preset_btn, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_align(s_preset_btn, LV_ALIGN_CENTER, 0, kCenterLineOffsetY);
  lv_obj_add_event_cb(s_preset_btn, on_preset_tap, LV_EVENT_CLICKED, nullptr);
  s_idle_preset = build_preset_readout(s_preset_btn);
  lv_obj_center(s_idle_preset.root);

  // Menu button — mirror of the Post pill on the opposite (left) end of the
  // center line. Outlined-capsule chrome in presets_screen::kColorMenu, with a
  // hamburger glyph + "Menu" label (mirroring the Back pill's chevron + label).
  // Lives in s_idle_group so it hides in post mode; opens the Presets screen.
  // Width + inset mirror the post-mode ✕ Cancel pill so the Menu pill sits in
  // exactly the same footprint Cancel occupies when the center line swaps modes.
  const lv_color_t menu_color = presets_screen::kColorMenu;
  s_menu_btn = lv_button_create(s_idle_group);
  lv_obj_set_size(s_menu_btn, kCancelBtnW, kPostBtnH);
  lv_obj_set_style_radius(s_menu_btn, kPostBtnH / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_menu_btn, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_menu_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_menu_btn, menu_color, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_menu_btn, kPostBtnStroke, LV_PART_MAIN);
  lv_obj_set_style_border_opa(s_menu_btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(s_menu_btn, LV_ALIGN_LEFT_MID, kCancelButtonX,
               kCenterLineOffsetY);
  lv_obj_set_ext_click_area(s_menu_btn, kPostBtnExtClick);
  lv_obj_add_event_cb(s_menu_btn, on_menu_tap, LV_EVENT_CLICKED, nullptr);

  // Hamburger glyph + "Menu" label in a centered row — mirrors the Back pill's
  // "chevron + Back" layout on the opposite end of the screen. The glyph widget
  // (size + painter) comes from ui_presets so the Menu icon travels with its
  // screen.
  lv_obj_t* menu_row = lv_obj_create(s_menu_btn);
  lv_obj_set_size(menu_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(menu_row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(menu_row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(menu_row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(menu_row, 8, LV_PART_MAIN);
  lv_obj_set_layout(menu_row, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(menu_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(menu_row, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(menu_row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(menu_row, LV_OBJ_FLAG_SCROLLABLE);

  presets_screen::build_menu_glyph(menu_row);

  lv_obj_t* menu_lbl = lv_label_create(menu_row);
  lv_obj_set_style_text_color(menu_lbl, menu_color, LV_PART_MAIN);
  lv_obj_set_style_text_font(menu_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(menu_lbl, "Menu");

  lv_obj_center(menu_row);
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

  // Transparent sub-container for the animated content only (brew block +
  // quality block). The center-line widgets built at the end of this function
  // stay on s_post_group so they switch instantly, not via the fade. Sized to
  // the top band (all brew + quality content sits above the climate separator),
  // so fading its opa invalidates only that band, not the full 466×466 panel.
  // At (0,0), so child coords match parenting on s_post_group.
  s_post_anim = lv_obj_create(s_post_group);
  lv_obj_set_size(s_post_anim, kScreen, kClimateSeparatorY);
  lv_obj_set_pos(s_post_anim, 0, 0);
  lv_obj_set_style_bg_opa(s_post_anim, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_post_anim, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_post_anim, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_post_anim, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(s_post_anim, LV_OBJ_FLAG_CLICKABLE);

  // Post-mode layout (replaces idle's climate strip in the top area; the
  // grind area below kGrinderSeparatorY is shared across modes):
  //
  //   top area (y=0..kClimateSeparatorY):
  //     BREW TIME caption (Mont 14, top-centered) + a row of
  //     (-) | big value (Mont 46) | (+) centered horizontally. First tap
  //     of either button promotes the value from "--" to the pre-seeded
  //     preset target; subsequent taps step ±1 s.
  //   climate separator at kClimateSeparatorY (shared, parented to `scr`
  //     in build_idle_group — see comment there).
  //   middle band (y=kClimateSeparatorY..kGrinderSeparatorY):
  //     Quality-section separator just below the climate cap, then
  //     QUALITY label LEFT + 5 stars centered, then Sour / Bitter pills
  //     horizontal below the stars (50% bigger than the prior vertical
  //     stack — see kPillW/H below; the pill row overlaps the right
  //     edge of the Submit button by a few pixels since the wider pills
  //     don't fit cleanly between ✕ and Submit and the user picked the
  //     "keep center buttons on the center line" layout).
  //   center line at kCenterLineY: ✕ (LEFT) · preset readout (CENTER) ·
  //     Submit (RIGHT). Identical to the old layout.

  // --- BREW TIME caption + (-)/value/(+) row ---
  constexpr int32_t kBrewCaptionTopY = 25;
  constexpr int32_t kBrewRowCenterY  = 74;
  constexpr int32_t kBrewBtnDX       = 90;   // (-)/(+) center distance from kCenter
  // The value + steppers center-align within s_post_anim, whose height is the
  // top band (kClimateSeparatorY), NOT the full screen — so the y offset is
  // measured from the band's center, not kCenter.
  constexpr int32_t kBrewRowAlignDY  = kBrewRowCenterY - kClimateSeparatorY / 2;

  s_brew_time_caption = lv_label_create(s_post_anim);
  lv_obj_set_style_text_color(s_brew_time_caption, kColorLabel, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_brew_time_caption, &lv_font_montserrat_14,
                             LV_PART_MAIN);
  lv_label_set_text(s_brew_time_caption, "BREW TIME");
  lv_obj_align(s_brew_time_caption, LV_ALIGN_TOP_MID, 0, kBrewCaptionTopY);

  // [ (-) value (+) ] row — the shared ui_stepper instance (MS46 value,
  // kPostBtnH discs at ±kBrewBtnDX). reset_post_form wires s_brew's min/max +
  // on_change and seeds value/touched; the stepper owns the readout + handlers.
  s_brew.min      = kBrewMinS;
  s_brew.max      = kBrewMaxS;
  s_brew.unit     = 's';
  s_brew.on_change = refresh_submit_enabled;
  const StepperCfg brew_cfg = {&lv_font_montserrat_46, kBrewBtnDX, kPostBtnH};
  lv_obj_t* brew_row = build_stepper(s_post_anim, &s_brew, brew_cfg);
  lv_obj_align(brew_row, LV_ALIGN_CENTER, 0, kBrewRowAlignDY);

  // --- Quality section (middle band) ---
  // 5 stars on the left, two side-by-side modifier pills (Sour | Bitter)
  // on the right, captioned by QUALITY and MODIFIERS respectively. Each
  // pill auto-sizes to its rendered text width plus kPillTextPaddingX of
  // total horizontal chrome, so "Sour" is narrower than "Bitter". The
  // whole [stars | gap | pills] group is then screen-centered so the gap
  // left of star 0 matches the gap right of the last pill.
  constexpr int32_t kQualityCaptionY = 125;
  constexpr int32_t kStarRowY        = 155;
  constexpr int32_t kStarRowW        =
      kMaxStars * kStarSize + (kMaxStars - 1) * kStarGap;

  constexpr int32_t kPillH            = (kPostBtnH * 4) / 5;
  constexpr int32_t kPillRowGap       = 6;
  constexpr int32_t kPillTextPaddingX = 27;   // total chrome around pill text (5 each side)
  // Hit-area padding on every side of each pill. Adjacent pills overlap
  // here — LVGL dispatches to the topmost-hit widget.
  constexpr int32_t kPillExtClick     = 10;
  constexpr int32_t kPillRowMidY      = 175;
  constexpr int32_t kPillRowY         = kPillRowMidY - kPillH / 2;
  constexpr int32_t kStarsToPillsGap  = 30;
  constexpr int32_t kPillButtonStroke = 2;

  struct PillCfg { const char* text; uint8_t mask; lv_obj_t** btn; lv_obj_t** lbl; };
  PillCfg pills[2] = {
      {"Sour",   storage::kTasteSour,   &s_sour_btn,   &s_sour_label},
      {"Bitter", storage::kTasteBitter, &s_bitter_btn, &s_bitter_label},
  };

  // Measure each pill's text width up front via a scratch Mont 24 label so
  // we can pin the centered group's x positions before building anything.
  int32_t pill_w[2];
  {
    lv_obj_t* scratch = lv_label_create(s_post_anim);
    lv_obj_set_style_text_font(scratch, &lv_font_montserrat_24, LV_PART_MAIN);
    for (int i = 0; i < 2; ++i) {
      lv_label_set_text(scratch, pills[i].text);
      lv_obj_update_layout(scratch);
      pill_w[i] = lv_obj_get_width(scratch) + kPillTextPaddingX;
    }
    lv_obj_delete(scratch);
  }
  const int32_t kPillRowW = pill_w[0] + kPillRowGap + pill_w[1];

  // Center the [stars | gap | pills] group so the screen-edge gaps match.
  const int32_t kStarRowX0 =
      (kScreen - kStarRowW - kStarsToPillsGap - kPillRowW) / 2;
  const int32_t kPillRowX0 =
      kStarRowX0 + kStarRowW + kStarsToPillsGap;

  // Caption anchors on the kQualityCaptionY baseline. QUALITY centers over
  // the middle star (index 2); MODIFIERS over the pill-row midpoint.
  const int32_t kMiddleStarCenterX =
      kStarRowX0 + 2 * (kStarSize + kStarGap) + kStarSize / 2;
  const int32_t kPillRowCenterX    = kPillRowX0 + kPillRowW / 2;

  // QUALITY + MODIFIERS captions — both Mont 14 on the same y baseline,
  // each centered on its content column. Use update_layout + set_pos so
  // we can horizontally center on a fixed x anchor (LV_ALIGN_TOP_MID
  // would center on the screen, not on the column).
  s_quality_caption = lv_label_create(s_post_anim);
  lv_obj_set_style_text_color(s_quality_caption, kColorLabel, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_quality_caption, &lv_font_montserrat_14,
                             LV_PART_MAIN);
  lv_label_set_text(s_quality_caption, "QUALITY");
  lv_obj_update_layout(s_quality_caption);
  lv_obj_set_pos(s_quality_caption,
                 kMiddleStarCenterX - lv_obj_get_width(s_quality_caption) / 2,
                 kQualityCaptionY);

  lv_obj_t* modifiers_caption = lv_label_create(s_post_anim);
  lv_obj_set_style_text_color(modifiers_caption, kColorLabel, LV_PART_MAIN);
  lv_obj_set_style_text_font(modifiers_caption, &lv_font_montserrat_14,
                             LV_PART_MAIN);
  lv_label_set_text(modifiers_caption, "TASTE");
  lv_obj_update_layout(modifiers_caption);
  lv_obj_set_pos(modifiers_caption,
                 kPillRowCenterX - lv_obj_get_width(modifiers_caption) / 2,
                 kQualityCaptionY);

  // Place each star icon directly on the post group (no per-star tap
  // parent). Tap + swipe are owned by a single overlay built next, which
  // dispatches to on_star_swipe based on indev x.
  for (uint8_t i = 0; i < kMaxStars; ++i) {
    s_star_icons[i] = make_star(s_post_anim, &s_star_states[i]);
    lv_obj_set_pos(s_star_icons[i],
                   kStarRowX0 + i * (kStarSize + kStarGap), kStarRowY);
  }

  // Star swipe overlay — transparent rect covering the row's full hit-area
  // extent (visible row + kStarExtClick on every side). Sits ABOVE the
  // star icons in z-order (created after them) so it captures
  // PRESSED/PRESSING before the icons see anything. The handler maps the
  // live indev x to a star count via threshold crossings — tap or drag,
  // both work the same way.
  lv_obj_t* star_swipe = lv_obj_create(s_post_anim);
  lv_obj_set_size(star_swipe,
                  kStarRowW + 2 * kStarExtClick,
                  kStarSize + 2 * kStarExtClick);
  lv_obj_set_pos(star_swipe,
                 kStarRowX0 - kStarExtClick,
                 kStarRowY - kStarExtClick);
  lv_obj_set_style_bg_opa(star_swipe, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(star_swipe, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(star_swipe, 0, LV_PART_MAIN);
  lv_obj_clear_flag(star_swipe, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(star_swipe, LV_OBJ_FLAG_CLICKABLE);
  for (auto code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING}) {
    lv_obj_add_event_cb(star_swipe, on_star_swipe, code, nullptr);
  }

  // Sour / Bitter pills, side by side, each sized to fit its own text.
  // Chrome (border + bg + text colors) is driven by refresh_taste_toggles.
  int32_t pill_x = kPillRowX0;
  for (int i = 0; i < 2; ++i) {
    lv_obj_t* b = lv_button_create(s_post_anim);
    lv_obj_set_size(b, pill_w[i], kPillH);
    lv_obj_set_style_radius(b, kPillH / 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, kPillButtonStroke, LV_PART_MAIN);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(b, pill_x, kPillRowY);
    pill_x += pill_w[i] + kPillRowGap;
    lv_obj_set_ext_click_area(b, kPillExtClick);
    lv_obj_add_event_cb(b, on_taste_tap, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(
                            static_cast<uintptr_t>(pills[i].mask)));
    lv_obj_t* lbl = lv_label_create(b);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_label_set_text(lbl, pills[i].text);
    lv_obj_center(lbl);
    *pills[i].btn = b;
    *pills[i].lbl = lbl;
  }

  // --- Center line: preset readout (left), ✕ (middle), Submit (right) ---
  // Preset is a two-line read-only label at the same LEFT_MID anchor as
  // idle's preset button so the readout doesn't jump on mode swap. ✕ is
  // centered on the line in Temperature red (outline-only, matches the
  // POST button's stroke pattern). Submit is the same outline pattern;
  // its border + label color is driven by refresh_submit_enabled and
  // toggles between kColorSubmitEnabled (humidity blue) and
  // kColorSubmitDisabled (muted gray).
  s_post_preset = build_preset_readout(s_post_group);
  lv_obj_align(s_post_preset.root, LV_ALIGN_CENTER, 0, kCenterLineOffsetY);

  s_cancel_btn = lv_button_create(s_post_group);
  lv_obj_set_size(s_cancel_btn, kCancelBtnW, kPostBtnH);
  lv_obj_set_style_radius(s_cancel_btn, kPostBtnH / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_cancel_btn, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_cancel_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_cancel_btn, kColorCancel, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_cancel_btn, kPostBtnStroke, LV_PART_MAIN);
  lv_obj_set_style_border_opa(s_cancel_btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(s_cancel_btn, LV_ALIGN_LEFT_MID, kCancelButtonX,
               kCenterLineOffsetY);
  lv_obj_set_ext_click_area(s_cancel_btn, kPostBtnExtClick);
  lv_obj_add_event_cb(s_cancel_btn, on_cancel_tap, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* cancel_lbl = lv_label_create(s_cancel_btn);
  lv_obj_set_style_text_color(cancel_lbl, kColorCancel, LV_PART_MAIN);
  lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(cancel_lbl, LV_SYMBOL_CLOSE " Cancel");
  lv_obj_center(cancel_lbl);

  s_submit_btn = lv_button_create(s_post_group);
  lv_obj_set_size(s_submit_btn, kSubmitBtnW, kPostBtnH);
  lv_obj_set_style_radius(s_submit_btn, kPostBtnH / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_submit_btn, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_submit_btn, 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_submit_btn, kColorSubmitDisabled, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_submit_btn, kPostBtnStroke, LV_PART_MAIN);
  lv_obj_set_style_border_opa(s_submit_btn, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(s_submit_btn, LV_ALIGN_RIGHT_MID, -kSubmitButtonX,
               kCenterLineOffsetY);
  lv_obj_set_ext_click_area(s_submit_btn, kPostBtnExtClick);
  s_submit_label = lv_label_create(s_submit_btn);
  lv_obj_set_style_text_color(s_submit_label, kColorSubmitDisabled, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_submit_label, &lv_font_montserrat_24,
                             LV_PART_MAIN);
  lv_label_set_text(s_submit_label, "Submit " LV_SYMBOL_RIGHT);
  lv_obj_center(s_submit_label);
  lv_obj_add_event_cb(s_submit_btn, on_submit, LV_EVENT_CLICKED, nullptr);
}

// The Presets screen (title + 3×3 grid + Back) is built by
// presets_screen::build() in ui_presets.cpp; start_report calls it and keeps the
// returned group handle in s_presets_group for the mode swap.

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
  // Presets group sits above the idle/post groups in z-order but stays hidden
  // until the Menu button opens it; built after them so it overlays cleanly. Its
  // Back pill is wired to on_back_tap (returns to Idle); each slot to on_slot_tap
  // (opens the editor).
  s_presets_group = presets_screen::build(scr, on_back_tap, on_slot_tap);
  // Edit group overlays the Presets grid (reached by tapping a slot); Cancel →
  // back to the grid, Save → persist + back to the grid, trash → confirm + clear.
  s_edit_group = preset_edit::build(scr, on_edit_cancel_tap, on_edit_save_tap,
                                    on_edit_delete_tap);

  // Mode-swap input block — full-screen transparent click-eater, hidden until a
  // cross-fade brings it to the foreground (see animate_mode_swap). Clickable so
  // it intercepts taps rather than passing them through to the fading groups.
  s_swap_block = lv_obj_create(scr);
  lv_obj_set_size(s_swap_block, kScreen, kScreen);
  lv_obj_set_pos(s_swap_block, 0, 0);
  lv_obj_set_style_bg_opa(s_swap_block, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_swap_block, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_swap_block, 0, LV_PART_MAIN);
  lv_obj_clear_flag(s_swap_block, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(s_swap_block, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(s_swap_block, LV_OBJ_FLAG_HIDDEN);

  // Popup (toast / tip / future confirm) — built last so its scrim + card sit
  // topmost in z-order, above even the mode-swap block. show_popup() reveals it.
  build_popup(scr);

  // Seed grind value from per-preset NVS; clamp in case older firmware stored
  // something outside the new ring's range.
  s_grind.value = std::clamp(presets::last_grind(presets::selected_id()),
                             kGrindMin, kGrindMax);
  // Pre-seed brew time at the current preset's target so the first tap of
  // (-)/(+) in post mode promotes the readout straight from "--" to the
  // user's normal target value. `touched` stays false until that tap.
  const auto seed_preset = presets::get(presets::selected_id());
  s_brew.value = std::clamp<uint8_t>(seed_preset.target_time_s,
                                     kBrewMinS, kBrewMaxS);

  refresh_preset_label();
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

  // Populate the section-swap fade sets now that every handle exists. Idle set =
  // the whole idle-visible world (climate band + center-line buttons + grinder
  // chrome); presets set = title + slots + Back, owned by ui_presets. The engine
  // skips any HIDDEN entry (e.g. the suggestion arrow/label before the model has
  // a suggestion).
  s_idle_fade_n = 0;
  for (lv_obj_t* w : {s_climate_anim, s_menu_btn, s_preset_btn, s_post_btn,
                      s_grind.widget, s_static_cursor, s_suggestion_arrow,
                      s_grind_value_label, s_grind_caption, s_suggestion_caption,
                      s_suggested_label}) {
    s_idle_fade[s_idle_fade_n++] = w;
  }
  s_presets_fade = presets_screen::fade_widgets(&s_presets_fade_n);
  s_edit_fade    = preset_edit::fade_widgets(&s_edit_fade_n);

  apply_mode();  // s_mode starts Idle; hides post + presets + edit groups

  // LVGL allocates every widget + style + the glyph cache from one fixed pool
  // (CONFIG_LV_MEM_SIZE_KILOBYTES). All screens are now built, so this is the
  // high-water mark — watch `used_pct` / `free_biggest` as more screens land; an
  // exhausted pool corrupts allocations and hangs the draw.
  lv_mem_monitor_t mon;
  lv_mem_monitor(&mon);
  ESP_LOGI(kTag, "lv_mem: used %u / %u B (%u%%), free_biggest %u B, frag %u%%",
           static_cast<unsigned>(mon.total_size - mon.free_size),
           static_cast<unsigned>(mon.total_size), static_cast<unsigned>(mon.used_pct),
           static_cast<unsigned>(mon.free_biggest_size),
           static_cast<unsigned>(mon.frag_pct));

  lv_timer_create(update_climate_strip, 1000, nullptr);

  display::unlock();
}

}  // namespace espressopost::ui
