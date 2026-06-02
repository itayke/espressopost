#include "ui_preset_edit.hpp"

#include <algorithm>
#include <cstdio>

#include "ui_stepper.hpp"
#include "ui_theme.hpp"

namespace espressopost::ui::preset_edit {
namespace {

// ===== EDIT PRESET TUNING ==================================================
// One bannered block for the whole screen so it's tuned in one place. Layout:
// title up top, then three identical steppers stacked down the center — Weight
// In, Weight Out, Brew Time — each a [ (-) value (+) ] row with its caption just
// above. Two vertical columns of color swatches flank them on the left/right
// edges, and the ✕ Cancel / Save › buttons sit on the bottom arc.

// Title.
constexpr int32_t kTitleTopY = 22;

// Weight steppers — value domain (grams). Defaults seed the first tap.
constexpr int32_t kDoseMin   = 5;
constexpr int32_t kDoseMax   = 50;
constexpr int32_t kDoseDef   = 18;
constexpr int32_t kYieldMin  = 10;
constexpr int32_t kYieldMax  = 100;
constexpr int32_t kYieldDef  = 36;

// Brew-time stepper — value domain (seconds).
constexpr int32_t kBrewTimeMin = 0;
constexpr int32_t kBrewTimeMax = 99;
constexpr int32_t kBrewTimeDef = 30;    // seeded so the first tap shows 30s

// Stepper geometry — the three rows share one shape. Each is centered on its
// *CenterY (so container top = CenterY - kStepperHalfH); the caption sits
// kCaptionDY above that center. Even pitch top to bottom; the bottom row's discs
// must clear the bottom pills (pill tops at ~y392).
constexpr int32_t kStepperBtnDX  = 86;   // discs ±this from row center
constexpr int32_t kStepperHalfH  = (kPostBtnH + 2 * kPostBtnExtClick) / 2;
constexpr int32_t kInCenterY     = 120;
constexpr int32_t kOutCenterY    = 230;
constexpr int32_t kBrewCenterY   = 340;
constexpr int32_t kCaptionDY     = 46;   // caption top above each stepper center
constexpr int32_t kStepperValueExtClick = 28;  // hit area around the "--"/value

// Color swatches — two vertical columns (5 each) down the left/right edges,
// flanking the steppers. Straight columns down the sides leave the whole height
// for the swatches, so they can run bigger than a single bottom row. The column
// x sits just outside the steppers' discs and just inside the round edge;
// kSwatchPitch is bounded so the top/bottom rows still clear the arc.
constexpr int32_t kNumSwatches   = 10;
constexpr int32_t kSwatchPerCol  = 5;
constexpr int32_t kSwatchDiam    = 40;
constexpr int32_t kSwatchSelDiam = 50;    // selected: 25% larger
constexpr int32_t kSwatchPitch   = 60;    // vertical center-to-center
constexpr int32_t kSwatchColX    = 70;    // left column center x (right mirrors)
constexpr int32_t kSwatchSelStroke = 2;   // white outline on the chosen swatch

// Bottom actions — a circular ✕ Cancel disc + a snug Save › pill, paired at
// center with a small gap (narrow buttons clear the round edge cleanly).
constexpr int32_t kBtnBottomInset = 16;
constexpr int32_t kCancelBtnDiam  = kPostBtnH;  // circular ✕ disc
constexpr int32_t kSaveBtnW       = 110;        // just wide enough for "Save ›"
constexpr int32_t kBtnGap         = 12;         // between Cancel and Save

// Palette — 10 distinct hues kept off max intensity (AMOLED burn-in / matches
// the kColorText grey at the end). gather() stores the chosen entry verbatim.
constexpr uint32_t kPalette[kNumSwatches] = {
    0xC88036,  // amber
    0xE07055,  // coral
    0xCC4444,  // red
    0xC056A0,  // magenta
    0x8060C0,  // violet
    0x60A8E0,  // blue
    0x40A0A0,  // teal
    0x5AA85A,  // green
    0x9CB84A,  // lime
    0xE0E0E0,  // light grey (the seeded default)
};

// Action-pill colors — own consts (Cancel = warm "undo", Save borrows the
// submit blue, disabled greys out).
const lv_color_t kColorCancel       = COLOR(0xE07055);
const lv_color_t kColorSaveEnabled  = COLOR(0x60A8E0);
const lv_color_t kColorSaveDisabled = kColorMuted3;
const lv_color_t kColorEditTitle    = kColorText;
const lv_color_t kColorCaption      = COLOR(0xB0B0B0);  // small all-caps headers
const lv_color_t kColorSwatchSel    = COLOR(0xFFFFFF);  // outline on the chosen swatch

// Shared value font for every editor readout (Weight In/Out + brew time).
const lv_font_t* const kEditValueFont = &lv_font_montserrat_36;
// ===========================================================================

// ---- State ----------------------------------------------------------------
StepperState s_in   = {};   // Weight In  (grams)
StepperState s_out  = {};   // Weight Out (grams)
StepperState s_time = {};   // Brew Time  (seconds)

lv_obj_t* s_title       = nullptr;
lv_obj_t* s_swatch[kNumSwatches] = {};
int32_t   s_swatch_cx[kNumSwatches] = {};
int32_t   s_swatch_cy[kNumSwatches] = {};
lv_obj_t* s_save_btn    = nullptr;

presets::Preset s_loaded        = {};   // the slot's prior data (zeros if empty)
bool            s_loaded_active = false;
int             s_color_idx     = -1;   // selected palette index, -1 = none

// 10 swatches + 9 chrome (title, 3 captions, 3 stepper rows, 2 pills).
lv_obj_t* s_fade[kNumSwatches + 9] = {};
int       s_fade_n = 0;

// ---- Helpers --------------------------------------------------------------
// Save lights only once a color is chosen AND all three steppers are set — both
// the outline AND the "Save ›" label recolor (the label was tinted to the
// disabled grey at build time, so it has to follow).
void refresh_save_enabled() {
  if (s_save_btn == nullptr) return;
  const bool ready = (s_color_idx >= 0) && s_in.touched && s_out.touched &&
                     s_time.touched;
  const lv_color_t c = ready ? kColorSaveEnabled : kColorSaveDisabled;
  lv_obj_set_style_border_color(s_save_btn, c, LV_PART_MAIN);
  lv_obj_t* lbl = lv_obj_get_child(s_save_btn, 0);
  if (lbl) lv_obj_set_style_text_color(lbl, c, LV_PART_MAIN);
}

// Tint the "PRESET N" title to the chosen swatch (or the neutral title color
// when nothing's picked yet).
void refresh_title_tint() {
  if (s_title == nullptr) return;
  lv_obj_set_style_text_color(
      s_title,
      (s_color_idx >= 0) ? lv_color_hex(kPalette[s_color_idx]) : kColorEditTitle,
      LV_PART_MAIN);
}

// Size each swatch (selected = larger) and re-center it on its arc point —
// LV_ALIGN_CENTER keeps it pinned to (cx, cy) as the size changes, so it grows
// about its own center.
void apply_swatch_visuals() {
  for (int i = 0; i < kNumSwatches; ++i) {
    if (s_swatch[i] == nullptr) continue;
    const bool    sel = (i == s_color_idx);
    const int32_t d   = sel ? kSwatchSelDiam : kSwatchDiam;
    lv_obj_set_size(s_swatch[i], d, d);
    lv_obj_set_style_radius(s_swatch[i], d / 2, LV_PART_MAIN);
    // White ring marks the chosen swatch (drawn inside, so the size is unchanged).
    lv_obj_set_style_border_width(s_swatch[i], sel ? kSwatchSelStroke : 0,
                                  LV_PART_MAIN);
    lv_obj_set_style_border_color(s_swatch[i], kColorSwatchSel, LV_PART_MAIN);
    lv_obj_set_style_border_opa(s_swatch[i], LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_align(s_swatch[i], LV_ALIGN_CENTER,
                 s_swatch_cx[i] - kCenter, s_swatch_cy[i] - kCenter);
  }
  refresh_title_tint();
}

void on_swatch_tap(lv_event_t* e) {
  const auto idx = static_cast<int>(reinterpret_cast<intptr_t>(
      lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(e)))));
  s_color_idx = idx;
  apply_swatch_visuals();
  refresh_save_enabled();
}

// Map a stored color back to its palette index, or -1 if it isn't one of ours
// (every color we save comes from the palette, so active slots always match).
int palette_index_of(uint32_t color) {
  for (int i = 0; i < kNumSwatches; ++i) {
    if ((kPalette[i] & 0x00FFFFFFu) == (color & 0x00FFFFFFu)) return i;
  }
  return -1;
}

// One outline action pill with an icon+text label, mirroring the post pills.
lv_obj_t* build_pill(lv_obj_t* parent, int32_t w, lv_color_t color,
                     const char* text, lv_event_cb_t cb, lv_align_t align,
                     int32_t dx) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, w, kPostBtnH);
  lv_obj_set_style_radius(b, kPostBtnH / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(b, color, LV_PART_MAIN);
  lv_obj_set_style_border_width(b, kPostBtnStroke, LV_PART_MAIN);
  lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(b, align, dx, -kBtnBottomInset);
  lv_obj_set_ext_click_area(b, kPostBtnExtClick);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* lbl = lv_label_create(b);
  lv_obj_set_style_text_color(lbl, color, LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(lbl, text);
  lv_obj_center(lbl);
  return b;
}

// A muted caption label (the small all-caps headers), placed by alignment.
lv_obj_t* build_caption(lv_obj_t* parent, const char* text, lv_align_t align,
                        int32_t dx, int32_t top_y) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_color(l, kColorCaption, LV_PART_MAIN);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(l, text);
  lv_obj_align(l, align, dx, top_y);
  return l;
}

// Build one captioned stepper centered on `center_y`: a "CAPTION" header above a
// [ (-) value (+) ] row. Wires `st` (min/max/unit/on_change set by the caller),
// widens the value's hit area, and returns the caption + row via out-params for
// the fade set.
void build_weight_stepper(lv_obj_t* parent, StepperState* st, const char* caption,
                          int32_t center_y, lv_obj_t** out_cap,
                          lv_obj_t** out_row) {
  *out_cap = build_caption(parent, caption, LV_ALIGN_TOP_MID, 0,
                           center_y - kCaptionDY);
  const StepperCfg cfg = {kEditValueFont, kStepperBtnDX, kPostBtnH};
  lv_obj_t* row = build_stepper(parent, st, cfg);
  lv_obj_align(row, LV_ALIGN_TOP_MID, 0, center_y - kStepperHalfH);
  // The "--"/value glyph is a small target — widen its hit area to land taps.
  lv_obj_set_ext_click_area(st->value_lbl, kStepperValueExtClick);
  *out_row = row;
}

}  // namespace

lv_obj_t* build(lv_obj_t* scr, lv_event_cb_t on_cancel, lv_event_cb_t on_save) {
  lv_obj_t* group = lv_obj_create(scr);
  lv_obj_set_size(group, kScreen, kScreen);
  lv_obj_set_pos(group, 0, 0);
  lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(group, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(group, 0, LV_PART_MAIN);
  lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(group, LV_OBJ_FLAG_CLICKABLE);

  // Title.
  s_title = lv_label_create(group);
  lv_obj_set_style_text_color(s_title, kColorEditTitle, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_title, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(s_title, "PRESET");
  lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, kTitleTopY);

  // Three captioned steppers down the center. min/max/unit/on_change are wired
  // here; load() seeds value/touched. on_change re-gates Save after every tap.
  s_in.min  = kDoseMin;   s_in.max  = kDoseMax;   s_in.unit  = 'g';
  s_out.min = kYieldMin;  s_out.max = kYieldMax;  s_out.unit = 'g';
  s_time.min = kBrewTimeMin; s_time.max = kBrewTimeMax; s_time.unit = 's';
  s_in.on_change = s_out.on_change = s_time.on_change = refresh_save_enabled;

  lv_obj_t* in_cap;   lv_obj_t* in_row;
  lv_obj_t* out_cap;  lv_obj_t* out_row;
  lv_obj_t* brew_cap; lv_obj_t* brew_row;
  build_weight_stepper(group, &s_in,   "WEIGHT IN",  kInCenterY,   &in_cap,   &in_row);
  build_weight_stepper(group, &s_out,  "WEIGHT OUT", kOutCenterY,  &out_cap,  &out_row);
  build_weight_stepper(group, &s_time, "BREW TIME",  kBrewCenterY, &brew_cap, &brew_row);

  // Color swatches — two vertical columns (left = 0..4, right = 5..9), each
  // centered vertically on screen at kSwatchPitch spacing. Left column hugs
  // kSwatchColX, right column mirrors it across center.
  const int32_t col_x[2]  = {kSwatchColX, kScreen - kSwatchColX};
  const int32_t col_top_y = kCenter - (kSwatchPerCol - 1) * kSwatchPitch / 2;
  for (int i = 0; i < kNumSwatches; ++i) {
    const int col = i / kSwatchPerCol;
    const int row = i % kSwatchPerCol;
    s_swatch_cx[i] = col_x[col];
    s_swatch_cy[i] = col_top_y + row * kSwatchPitch;

    lv_obj_t* sw = lv_button_create(group);
    lv_obj_set_style_bg_color(sw, lv_color_hex(kPalette[i]), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_MAIN);
    lv_obj_set_user_data(sw, reinterpret_cast<void*>(static_cast<intptr_t>(i)));
    lv_obj_add_event_cb(sw, on_swatch_tap, LV_EVENT_CLICKED, nullptr);
    s_swatch[i] = sw;
  }
  apply_swatch_visuals();  // sizes + centers them (none selected yet)

  // Bottom actions — ✕ disc (left) + Save › pill (right), centered as a pair with
  // a kBtnGap gap. The dx offsets keep the combined [disc | gap | pill] block
  // centered on screen despite the two differing widths.
  const int32_t cancel_dx = -(kBtnGap + kSaveBtnW) / 2;
  const int32_t save_dx   = (kCancelBtnDiam + kBtnGap) / 2;
  lv_obj_t* cancel_btn =
      build_pill(group, kCancelBtnDiam, kColorCancel, LV_SYMBOL_CLOSE,
                 on_cancel, LV_ALIGN_BOTTOM_MID, cancel_dx);
  s_save_btn = build_pill(group, kSaveBtnW, kColorSaveDisabled,
                          "Save " LV_SYMBOL_RIGHT, on_save,
                          LV_ALIGN_BOTTOM_MID, save_dx);

  // Fade set — every visible widget.
  s_fade_n = 0;
  s_fade[s_fade_n++] = s_title;
  s_fade[s_fade_n++] = in_cap;
  s_fade[s_fade_n++] = in_row;
  s_fade[s_fade_n++] = out_cap;
  s_fade[s_fade_n++] = out_row;
  s_fade[s_fade_n++] = brew_cap;
  s_fade[s_fade_n++] = brew_row;
  for (int i = 0; i < kNumSwatches; ++i) s_fade[s_fade_n++] = s_swatch[i];
  s_fade[s_fade_n++] = cancel_btn;
  s_fade[s_fade_n++] = s_save_btn;

  return group;
}

void load(uint8_t slot) {
  s_loaded_active = presets::is_active(slot);
  s_loaded        = s_loaded_active ? presets::get(slot) : presets::Preset{};

  char title[16];
  std::snprintf(title, sizeof(title), "PRESET %u",
                static_cast<unsigned>(slot + 1));
  lv_label_set_text(s_title, title);

  // Each stepper: an existing preset's value is already "set"; a new slot starts
  // "--" but pre-seeded so the first tap reveals a sane default.
  s_in.touched  = s_loaded_active;
  s_in.value    = std::clamp<int>(
      s_loaded_active ? s_loaded.dose_g : kDoseDef, kDoseMin, kDoseMax);
  s_out.touched = s_loaded_active;
  s_out.value   = std::clamp<int>(
      s_loaded_active ? s_loaded.yield_g : kYieldDef, kYieldMin, kYieldMax);
  s_time.touched = s_loaded_active;
  s_time.value   = std::clamp<int>(
      s_loaded_active ? s_loaded.target_time_s : kBrewTimeDef,
      kBrewTimeMin, kBrewTimeMax);
  stepper_refresh(&s_in);
  stepper_refresh(&s_out);
  stepper_refresh(&s_time);

  // Color: match the stored accent to a swatch (active slots always match).
  s_color_idx = s_loaded_active ? palette_index_of(s_loaded.color) : -1;
  apply_swatch_visuals();
  refresh_save_enabled();
}

bool gather(presets::Preset* out) {
  if (s_color_idx < 0 || !s_in.touched || !s_out.touched || !s_time.touched)
    return false;
  presets::Preset p = s_loaded;  // preserve grind_anchor / name for edits
  if (!s_loaded_active) {
    p = presets::Preset{};
    p.grind_anchor = 5.2f;  // model baseline for a freshly created slot
    p.name[0]      = '\0';
  }
  p.dose_g        = s_in.value;
  p.yield_g       = s_out.value;
  p.target_time_s = s_time.value;
  p.color         = kPalette[s_color_idx];
  *out = p;
  return true;
}

lv_obj_t* const* fade_widgets(int* out_n) {
  *out_n = s_fade_n;
  return s_fade;
}

}  // namespace espressopost::ui::preset_edit
