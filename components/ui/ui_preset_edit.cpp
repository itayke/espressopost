#include "ui_preset_edit.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "ui_bar.hpp"
#include "ui_theme.hpp"
#include "ui_time_stepper.hpp"

namespace espressopost::ui::preset_edit {
namespace {

// ===== EDIT PRESET TUNING ==================================================
// One bannered block for the whole screen so it's tuned in one place. Geometry
// notes: the two weight bars own the vertical-middle band (widest chord), so the
// 5+5 color swatches that flank them on the arcs hug the screen edge there and
// the near-full-width strips clear them. The swatches that curve inward (top /
// bottom of each column) sit in the rows above/below the bars, where nothing
// else lives. Retune strip y + half-width together with the swatch rows.

// Title.
constexpr int32_t kTitleTopY = 24;

// Weight bars — value domain. "Density 10g" window: default 18 shows 13..23.
constexpr int32_t kDoseMin   = 5;
constexpr int32_t kDoseMax   = 50;
constexpr int32_t kDoseDef   = 18;
constexpr int32_t kYieldMin  = 10;
constexpr int32_t kYieldMax  = 100;
constexpr int32_t kYieldDef  = 36;
constexpr float   kWeightWindowHalf = 5.0f;  // value units from cursor to edge

// Weight bars — geometry. Near-full-width (a hair under the grind bar's
// kBarHalfWidth) so the arc swatches clear at the middle rows.
constexpr int32_t kWeightHalfWidth = 150;
constexpr int32_t kInStripY        = 156;
constexpr int32_t kOutStripY       = 236;
constexpr int32_t kInCaptionY      = 88;    // "WEIGHT IN" caption top inset
constexpr int32_t kInValueY        = 110;   // "18g" value (MS24) center-ish
constexpr int32_t kOutCaptionY     = 174;
constexpr int32_t kOutValueY       = 196;
// Drag hit-bands (screen y). Must not overlap — a press picks at most one bar.
constexpr int32_t kInBandTop    = 118;
constexpr int32_t kInBandBottom = 192;
constexpr int32_t kOutBandTop   = 194;
constexpr int32_t kOutBandBottom = 278;

// Brew-time stepper — compact (MS24 value), discs at kPostBtnH. Sits just above
// the bottom pills (disc bottom must clear the pill tops at ~y392).
constexpr int32_t kBrewCaptionY  = 260;
constexpr int32_t kBrewStepperY  = 278;   // container top inset
constexpr int32_t kBrewBtnDX     = 86;
constexpr int32_t kBrewTimeMin   = 0;
constexpr int32_t kBrewTimeMax   = 99;
constexpr int32_t kBrewTimeDef   = 30;    // seeded so the first tap shows 30s

// Color swatches — 5 down each arc, centers evenly spaced about screen center.
constexpr int32_t kSwatchDiam    = (kPostBtnH * 3) / 4;   // ~25% under button height
constexpr int32_t kSwatchSelDiam = (kSwatchDiam * 5) / 4;  // selected: 25% larger
constexpr int32_t kSwatchEdgeGap = 6;     // inset from the round edge
constexpr int32_t kSwatchEndInward = 10;  // top/bottom rows nudged toward center
constexpr int32_t kSwatchTopY    = 94;    // top swatch center y (left+right rows)
constexpr int32_t kSwatchPitch   = 67;    // center-to-center vertical spacing
                                          // (bottom row clears the pills at ~y392)
constexpr int32_t kSwatchPerSide = 5;
constexpr int32_t kNumSwatches   = 10;

// Bottom action pills — same chrome as the post ✕ Cancel / Submit › pills, but
// paired at center (a wide pill at the far corners gets clipped by the round
// edge) with a minimal gap between them.
constexpr int32_t kBtnBottomInset = 16;
constexpr int32_t kEditBtnW       = kPostBtnW + 20;  // matches the post pills
constexpr int32_t kBtnGap         = 10;   // between the centered Cancel / Save

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
const lv_color_t kColorWeight       = kColorText;   // weight value readouts
const lv_color_t kColorCaption      = COLOR(0xB0B0B0);  // small all-caps headers
// ===========================================================================

// ---- State ----------------------------------------------------------------
BarState         s_in   = {};   // Weight In
BarState         s_out  = {};   // Weight Out
TimeStepperState s_time = {};

BarSpec s_in_spec = {
    /*min*/ kDoseMin, /*max*/ kDoseMax, /*step*/ 1.0f,
    /*visible_half_range*/ kWeightWindowHalf,
    /*half_width*/ kWeightHalfWidth, /*center_x*/ kCenter, /*y*/ kInStripY,
    /*y_band_top*/ kInBandTop, /*y_band_bottom*/ kInBandBottom,
    /*big_every*/ 10, /*mid_every*/ 5, /*small_every*/ 0, /*tick_unit*/ 1.0f,
};
BarSpec s_out_spec = {
    /*min*/ kYieldMin, /*max*/ kYieldMax, /*step*/ 1.0f,
    /*visible_half_range*/ kWeightWindowHalf,
    /*half_width*/ kWeightHalfWidth, /*center_x*/ kCenter, /*y*/ kOutStripY,
    /*y_band_top*/ kOutBandTop, /*y_band_bottom*/ kOutBandBottom,
    /*big_every*/ 10, /*mid_every*/ 5, /*small_every*/ 0, /*tick_unit*/ 1.0f,
};

lv_obj_t* s_title       = nullptr;
lv_obj_t* s_in_value    = nullptr;
lv_obj_t* s_out_value   = nullptr;
lv_obj_t* s_swatch[kNumSwatches] = {};
int32_t   s_swatch_cx[kNumSwatches] = {};
int32_t   s_swatch_cy[kNumSwatches] = {};
lv_obj_t* s_save_btn    = nullptr;

presets::Preset s_loaded        = {};   // the slot's prior data (zeros if empty)
bool            s_loaded_active = false;
int             s_color_idx     = -1;   // selected palette index, -1 = none

// 10 swatches + 11 chrome (title, 2 captions, 2 values, 2 bars, brew caption +
// stepper, 2 pills).
lv_obj_t* s_fade[kNumSwatches + 11] = {};
int       s_fade_n = 0;

// ---- Helpers --------------------------------------------------------------
void set_weight_label(lv_obj_t* lbl, float v) {
  if (lbl == nullptr) return;
  char buf[8];
  std::snprintf(buf, sizeof(buf), "%ug",
                static_cast<unsigned>(std::lround(v)));
  lv_label_set_text(lbl, buf);
}

// Update the value readout AND invalidate the strip so its ticks scroll with the
// drag (the bar engine reads value at paint time but doesn't self-invalidate).
void in_on_change(BarState* s) {
  set_weight_label(s_in_value, s->value);
  if (s->widget) lv_obj_invalidate(s->widget);
}
void out_on_change(BarState* s) {
  set_weight_label(s_out_value, s->value);
  if (s->widget) lv_obj_invalidate(s->widget);
}

// Overlay forwarder — routes a touch to whichever weight bar's y-band it lands
// in (each bar ignores presses outside its band).
void on_weight_overlay_event(lv_event_t* e) {
  bar_dispatch_event(e, &s_in);
  bar_dispatch_event(e, &s_out);
}

// Save lights only once a color is chosen AND the brew time is set.
void refresh_save_enabled() {
  if (s_save_btn == nullptr) return;
  const bool ready = (s_color_idx >= 0) && s_time.touched;
  lv_obj_set_style_border_color(
      s_save_btn, ready ? kColorSaveEnabled : kColorSaveDisabled, LV_PART_MAIN);
}

// Size each swatch (selected = larger) and re-center it on its arc point —
// LV_ALIGN_CENTER keeps it pinned to (cx, cy) as the size changes, so it grows
// about its own center.
void apply_swatch_visuals() {
  for (int i = 0; i < kNumSwatches; ++i) {
    if (s_swatch[i] == nullptr) continue;
    const int32_t d = (i == s_color_idx) ? kSwatchSelDiam : kSwatchDiam;
    lv_obj_set_size(s_swatch[i], d, d);
    lv_obj_set_style_radius(s_swatch[i], d / 2, LV_PART_MAIN);
    lv_obj_align(s_swatch[i], LV_ALIGN_CENTER,
                 s_swatch_cx[i] - kCenter, s_swatch_cy[i] - kCenter);
  }
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

// A muted caption label (the small all-caps headers above each control).
lv_obj_t* build_caption(lv_obj_t* parent, const char* text, int32_t top_y) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_color(l, kColorCaption, LV_PART_MAIN);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(l, text);
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, top_y);
  return l;
}

lv_obj_t* build_weight_value(lv_obj_t* parent, int32_t top_y) {
  lv_obj_t* l = lv_label_create(parent);
  lv_obj_set_style_text_color(l, kColorWeight, LV_PART_MAIN);
  lv_obj_set_style_text_font(l, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(l, "--");
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, top_y);
  return l;
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

  // Transparent full-screen overlay for the weight-bar drags. Added FIRST so it
  // sits behind everything else — the (non-clickable) bar widgets fall through
  // to it, while the clickable swatches / discs / pills on top take their own
  // taps. on_weight_overlay_event hit-tests each bar's y-band.
  lv_obj_t* overlay = lv_obj_create(group);
  lv_obj_set_size(overlay, kScreen, kScreen);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
  for (auto code : {LV_EVENT_PRESSED, LV_EVENT_PRESSING, LV_EVENT_RELEASED,
                    LV_EVENT_PRESS_LOST}) {
    lv_obj_add_event_cb(overlay, on_weight_overlay_event, code, nullptr);
  }

  // Title.
  s_title = lv_label_create(group);
  lv_obj_set_style_text_color(s_title, kColorEditTitle, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_title, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(s_title, "EDIT PRESET");
  lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, kTitleTopY);

  // Weight In.
  lv_obj_t* in_cap = build_caption(group, "WEIGHT IN", kInCaptionY);
  s_in_value       = build_weight_value(group, kInValueY);
  s_in.spec        = &s_in_spec;
  s_in.on_change   = in_on_change;
  s_in.widget      = make_bar_widget(group, &s_in);

  // Weight Out.
  lv_obj_t* out_cap = build_caption(group, "WEIGHT OUT", kOutCaptionY);
  s_out_value       = build_weight_value(group, kOutValueY);
  s_out.spec        = &s_out_spec;
  s_out.on_change   = out_on_change;
  s_out.widget      = make_bar_widget(group, &s_out);

  // Brew time.
  lv_obj_t* brew_cap = build_caption(group, "BREW TIME", kBrewCaptionY);
  s_time.min_s       = kBrewTimeMin;
  s_time.max_s       = kBrewTimeMax;
  s_time.on_change   = refresh_save_enabled;
  const TimeStepperCfg brew_cfg = {&lv_font_montserrat_24, kBrewBtnDX, kPostBtnH};
  lv_obj_t* brew_row = build_time_stepper(group, &s_time, brew_cfg);
  lv_obj_align(brew_row, LV_ALIGN_TOP_MID, 0, kBrewStepperY);

  // Color swatches — 5 down each arc. cx hugs the round edge (inset by the
  // swatch radius + kSwatchEdgeGap) so the center rows clear the weight bars.
  for (int i = 0; i < kNumSwatches; ++i) {
    const bool left = (i < kSwatchPerSide);
    const int  row  = left ? i : (i - kSwatchPerSide);
    const int32_t cy = kSwatchTopY + row * kSwatchPitch;
    const float   dy = static_cast<float>(cy - kCenter);
    const float   chord_half =
        std::sqrt(static_cast<float>(kCenter) * kCenter - dy * dy);
    const int32_t inset = static_cast<int32_t>(std::lround(
        chord_half)) - kSwatchDiam / 2 - kSwatchEdgeGap;
    int32_t cx = left ? (kCenter - inset) : (kCenter + inset);
    // Pull the top + bottom rows of each column a touch further toward center so
    // they don't crowd the rim where the arc curves in hardest.
    if (row == 0 || row == kSwatchPerSide - 1) {
      cx += left ? kSwatchEndInward : -kSwatchEndInward;
    }
    s_swatch_cx[i] = cx;
    s_swatch_cy[i] = cy;

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

  // Bottom pills — ✕ Cancel + Save › paired at center, gap kBtnGap between them.
  const int32_t btn_dx = kEditBtnW / 2 + kBtnGap / 2;
  lv_obj_t* cancel_btn =
      build_pill(group, kEditBtnW, kColorCancel, LV_SYMBOL_CLOSE " Cancel",
                 on_cancel, LV_ALIGN_BOTTOM_MID, -btn_dx);
  s_save_btn = build_pill(group, kEditBtnW, kColorSaveDisabled,
                          "Save " LV_SYMBOL_RIGHT, on_save,
                          LV_ALIGN_BOTTOM_MID, +btn_dx);

  // Fade set — every visible widget (overlay stays transparent, excluded).
  s_fade_n = 0;
  s_fade[s_fade_n++] = s_title;
  s_fade[s_fade_n++] = in_cap;
  s_fade[s_fade_n++] = s_in_value;
  s_fade[s_fade_n++] = s_in.widget;
  s_fade[s_fade_n++] = out_cap;
  s_fade[s_fade_n++] = s_out_value;
  s_fade[s_fade_n++] = s_out.widget;
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

  char title[20];
  std::snprintf(title, sizeof(title), "EDIT PRESET %u",
                static_cast<unsigned>(slot + 1));
  lv_label_set_text(s_title, title);

  // Weight bars: existing values (clamped) or defaults.
  s_in.value = std::clamp<float>(
      s_loaded_active ? s_loaded.dose_g : kDoseDef, kDoseMin, kDoseMax);
  s_out.value = std::clamp<float>(
      s_loaded_active ? s_loaded.yield_g : kYieldDef, kYieldMin, kYieldMax);
  set_weight_label(s_in_value, s_in.value);
  set_weight_label(s_out_value, s_out.value);
  if (s_in.widget)  lv_obj_invalidate(s_in.widget);
  if (s_out.widget) lv_obj_invalidate(s_out.widget);

  // Brew time: an existing preset's time is already "set"; a new slot starts
  // "--" but pre-seeded so the first tap reveals a sane value.
  s_time.touched = s_loaded_active;
  s_time.value_s = std::clamp<int>(
      s_loaded_active ? s_loaded.target_time_s : kBrewTimeDef,
      kBrewTimeMin, kBrewTimeMax);
  time_stepper_refresh(&s_time);

  // Color: match the stored accent to a swatch (active slots always match).
  s_color_idx = s_loaded_active ? palette_index_of(s_loaded.color) : -1;
  apply_swatch_visuals();
  refresh_save_enabled();
}

bool gather(presets::Preset* out) {
  if (s_color_idx < 0 || !s_time.touched) return false;
  presets::Preset p = s_loaded;  // preserve grind_anchor / name for edits
  if (!s_loaded_active) {
    p = presets::Preset{};
    p.grind_anchor = 5.2f;  // model baseline for a freshly created slot
    p.name[0]      = '\0';
  }
  p.dose_g        = static_cast<uint8_t>(std::lround(s_in.value));
  p.yield_g       = static_cast<uint8_t>(std::lround(s_out.value));
  p.target_time_s = s_time.value_s;
  p.color         = kPalette[s_color_idx];
  *out = p;
  return true;
}

lv_obj_t* const* fade_widgets(int* out_n) {
  *out_n = s_fade_n;
  return s_fade;
}

}  // namespace espressopost::ui::preset_edit
