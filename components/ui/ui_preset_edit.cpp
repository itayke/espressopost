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
constexpr int32_t kInCenterY     = 118;
constexpr int32_t kOutCenterY    = 222;
constexpr int32_t kBrewCenterY   = 326;
constexpr int32_t kCaptionDY     = 46;   // caption top above each stepper center
constexpr int32_t kStepperValueExtClick = 28;  // hit area around the "--"/value

// Down-arrow between the two weight steppers — the readout's "→" glyph rotated
// vertical, centered between the In and Out rows. Marks the In → Out flow.
constexpr int32_t kWeightArrowW = 12;
constexpr int32_t kWeightArrowH = 16;
constexpr int32_t kWeightArrowY = (kInCenterY + kOutCenterY) / 2 - 14;  // row midpoint

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

// Bottom actions — a row of [ 🗑 trash | ✕ Cancel | Save › ], centered as one
// block with even gaps. The trash disc only shows for an existing slot (load()
// toggles it and re-centers the row via layout_bottom_row): with the trash the
// block is all three, without it Cancel/Save center as a pair. kBtnBottomInset is
// raised enough that the wider three-button block still clears the round bottom
// arc.
constexpr int32_t kBtnBottomInset = 26;
constexpr int32_t kCancelBtnDiam  = kPostBtnH;  // circular ✕ disc
constexpr int32_t kSaveBtnW       = 110;        // just wide enough for "Save ›"
constexpr int32_t kBtnGap         = 12;         // between adjacent bottom buttons

// Delete affordance — a small circular trash disc at the bottom, left of Cancel.
// Built once; load() shows it only for an existing (active) slot, since a fresh
// slot has nothing to delete. ui_report owns the CLICKED handler (the confirm
// popup + the actual clear).
constexpr int32_t kDeleteBtnDiam = kPostBtnH;
constexpr int32_t kDeleteBtnBottomInset = kBtnBottomInset + (kPostBtnH - kDeleteBtnDiam) / 2;

// Palette — 10 distinct hues kept off max intensity (AMOLED burn-in / matches
// the kColorText grey at the end). gather() stores the chosen entry verbatim.
constexpr uint32_t kPalette[kNumSwatches] = {
    0xE0E0E0,  // default
    0xD4B358,
    0xC08A4E,
    0x8C6239,
    0x5C3A21, 
    0xAF3C1F,
    0xC056A0,
    0x8060C0,
    0x40A0A0,
    0x9CB84A,
};

// Action-pill colors — own consts (Cancel = warm "undo", Save borrows the
// submit blue, disabled greys out).
const lv_color_t kColorCancel       = COLOR(0xE07055);
const lv_color_t kColorSaveEnabled  = COLOR(0x60A8E0);
const lv_color_t kColorSaveDisabled = kColorMuted3;
const lv_color_t kColorDelete       = COLOR(0x5A1207);  // trash disc
const lv_color_t kColorEditTitle    = kColorText;
const lv_color_t kColorCaption      = COLOR(0xB0B0B0);  // small all-caps headers
const lv_color_t kColorSwatchSel    = COLOR(0xFFFFFF);  // outline on the chosen swatch
const lv_color_t kColorWeightArrow  = kColorCaption;    // In→Out flow arrow

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
lv_obj_t* s_cancel_btn  = nullptr;
lv_obj_t* s_save_btn    = nullptr;
lv_obj_t* s_delete_btn  = nullptr;

presets::Preset s_loaded        = {};   // the slot's prior data (zeros if empty)
bool            s_loaded_active = false;
int             s_color_idx     = -1;   // selected palette index, -1 = none
bool            s_dirty         = false;  // a field changed since load → Save armed

// 10 swatches + 11 chrome (title, 3 captions, 3 stepper rows, In→Out arrow,
// 2 bottom pills, trash disc).
lv_obj_t* s_fade[kNumSwatches + 11] = {};
int       s_fade_n = 0;

// ---- Helpers --------------------------------------------------------------
// Save lights only once the form is complete (a color chosen AND all three
// steppers set) AND something has actually changed since load — so editing an
// existing preset keeps Save dark until the first edit. Both the outline AND the
// "Save ›" label recolor (the label was tinted to the disabled grey at build
// time, so it has to follow).
void refresh_save_enabled() {
  if (s_save_btn == nullptr) return;
  const bool ready = s_dirty && (s_color_idx >= 0) && s_in.touched &&
                     s_out.touched && s_time.touched;
  const lv_color_t c = ready ? kColorSaveEnabled : kColorSaveDisabled;
  lv_obj_set_style_border_color(s_save_btn, c, LV_PART_MAIN);
  lv_obj_t* lbl = lv_obj_get_child(s_save_btn, 0);
  if (lbl) lv_obj_set_style_text_color(lbl, c, LV_PART_MAIN);
}

// Any edit (a stepper tap or a swatch pick) arms Save. Wired as the steppers'
// on_change and called from the swatch handler.
void mark_dirty() {
  s_dirty = true;
  refresh_save_enabled();
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
  mark_dirty();
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

// Short downward arrow (the readout's right-arrow rotated vertical): a stem plus
// a V head opening down, centered on (kCenter, kWeightArrowY). Custom lv_line
// strokes (house style), so it fades cleanly with the rest.
lv_obj_t* build_weight_arrow(lv_obj_t* parent) {
  constexpr int32_t kStroke = 3;
  constexpr float   midX    = kWeightArrowW / 2.0f;  // LV_USE_FLOAT is on
  // Static so the vertex buffers outlive the call — lv_line keeps the pointer.
  static lv_point_precise_t stem_pts[] = {
      {midX, 0},
      {midX, kWeightArrowH - 4},
  };
  static lv_point_precise_t head_pts[] = {
      {2,                 kWeightArrowH - 4},
      {midX,              kWeightArrowH - 1},
      {kWeightArrowW - 2, kWeightArrowH - 4},
  };

  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_set_size(c, kWeightArrowW, kWeightArrowH);
  lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(c, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(c, 0, LV_PART_MAIN);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

  auto stroke = [&](const lv_point_precise_t* pts, uint32_t n) {
    lv_obj_t* line = lv_line_create(c);
    lv_line_set_points(line, pts, n);
    lv_obj_set_style_line_color(line, kColorWeightArrow, LV_PART_MAIN);
    lv_obj_set_style_line_width(line, kStroke, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
  };
  stroke(stem_pts, 2);
  stroke(head_pts, 3);
  lv_obj_align(c, LV_ALIGN_TOP_MID, 0, kWeightArrowY - kWeightArrowH / 2);
  return c;
}

// Outline trash disc for the bottom row. Same chrome as the bottom pills
// (transparent fill + red stroke + centered glyph), just circular and smaller.
// Placed at `dx` along the bottom arc (left of Cancel). CLICKED → the injected
// on_delete; load() toggles its visibility per slot.
lv_obj_t* build_delete_btn(lv_obj_t* parent, lv_event_cb_t on_delete, int32_t dx) {
  lv_obj_t* b = lv_button_create(parent);
  lv_obj_set_size(b, kDeleteBtnDiam, kDeleteBtnDiam);
  lv_obj_set_style_radius(b, kDeleteBtnDiam / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(b, kColorDelete, LV_PART_MAIN);
  lv_obj_set_style_border_width(b, kPostBtnStroke, LV_PART_MAIN);
  lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(b, LV_ALIGN_BOTTOM_MID, dx, -kDeleteBtnBottomInset);
  lv_obj_set_ext_click_area(b, kPostBtnExtClick);
  lv_obj_add_event_cb(b, on_delete, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* lbl = lv_label_create(b);
  lv_obj_set_style_text_color(lbl, kColorDelete, LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(lbl, LV_SYMBOL_TRASH);
  lv_obj_center(lbl);
  return b;
}

// Position the bottom row for the current button set. With the trash present the
// block is [ trash | Cancel | Save ]; without it (a fresh slot has nothing to
// delete) Cancel/Save center as a pair, so they don't sit skewed around an empty
// trash slot. Called from load() once the slot's active state is known.
void layout_bottom_row(bool with_delete) {
  const int32_t lead  = with_delete ? kDeleteBtnDiam + kBtnGap : 0;
  const int32_t row_w = lead + kCancelBtnDiam + kBtnGap + kSaveBtnW;
  const int32_t left  = -row_w / 2;  // block's left edge, from screen center
  if (with_delete) {
    lv_obj_align(s_delete_btn, LV_ALIGN_BOTTOM_MID, left + kDeleteBtnDiam / 2,
                 -kDeleteBtnBottomInset);
  }
  lv_obj_align(s_cancel_btn, LV_ALIGN_BOTTOM_MID, left + lead + kCancelBtnDiam / 2,
               -kBtnBottomInset);
  lv_obj_align(s_save_btn, LV_ALIGN_BOTTOM_MID, row_w / 2 - kSaveBtnW / 2,
               -kBtnBottomInset);
}

}  // namespace

lv_obj_t* build(lv_obj_t* scr, lv_event_cb_t on_cancel, lv_event_cb_t on_save,
                lv_event_cb_t on_delete) {
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
  s_in.on_change = s_out.on_change = s_time.on_change = mark_dirty;

  lv_obj_t* in_cap;   lv_obj_t* in_row;
  lv_obj_t* out_cap;  lv_obj_t* out_row;
  lv_obj_t* brew_cap; lv_obj_t* brew_row;
  build_weight_stepper(group, &s_in,   "WEIGHT IN",  kInCenterY,   &in_cap,   &in_row);
  build_weight_stepper(group, &s_out,  "WEIGHT OUT", kOutCenterY,  &out_cap,  &out_row);
  build_weight_stepper(group, &s_time, "BREW TIME",  kBrewCenterY, &brew_cap, &brew_row);
  lv_obj_t* flow_arrow = build_weight_arrow(group);

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

  // Bottom actions — [ 🗑 trash | ✕ Cancel | Save › ]. layout_bottom_row() owns
  // the dx offsets and reserves the trash slot only when it's shown, so build
  // here at a placeholder dx and lay out for real below / on each load().
  s_cancel_btn = build_pill(group, kCancelBtnDiam, kColorCancel, LV_SYMBOL_CLOSE,
                            on_cancel, LV_ALIGN_BOTTOM_MID, 0);
  s_save_btn = build_pill(group, kSaveBtnW, kColorSaveDisabled,
                          "Save " LV_SYMBOL_RIGHT, on_save,
                          LV_ALIGN_BOTTOM_MID, 0);

  // Trash disc — hidden until load() decides (only existing slots can delete).
  s_delete_btn = build_delete_btn(group, on_delete, 0);
  lv_obj_add_flag(s_delete_btn, LV_OBJ_FLAG_HIDDEN);
  layout_bottom_row(false);  // default to the no-trash pair; load() re-lays out

  // Fade set — every visible widget.
  s_fade_n = 0;
  s_fade[s_fade_n++] = s_title;
  s_fade[s_fade_n++] = in_cap;
  s_fade[s_fade_n++] = in_row;
  s_fade[s_fade_n++] = flow_arrow;
  s_fade[s_fade_n++] = out_cap;
  s_fade[s_fade_n++] = out_row;
  s_fade[s_fade_n++] = brew_cap;
  s_fade[s_fade_n++] = brew_row;
  for (int i = 0; i < kNumSwatches; ++i) s_fade[s_fade_n++] = s_swatch[i];
  s_fade[s_fade_n++] = s_cancel_btn;
  s_fade[s_fade_n++] = s_save_btn;
  s_fade[s_fade_n++] = s_delete_btn;

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

  // Trash disc only makes sense for an existing slot — a brand-new one has
  // nothing to delete yet. Re-center the bottom row to match (reserving the trash
  // slot only when it's shown).
  if (s_loaded_active) lv_obj_remove_flag(s_delete_btn, LV_OBJ_FLAG_HIDDEN);
  else                 lv_obj_add_flag(s_delete_btn, LV_OBJ_FLAG_HIDDEN);
  layout_bottom_row(s_loaded_active);

  // Color: match the stored accent to a swatch (active slots always match).
  s_color_idx = s_loaded_active ? palette_index_of(s_loaded.color) : -1;
  apply_swatch_visuals();

  // Fresh form — Save stays dark until the first edit (an existing preset loads
  // complete + valid, so without this it would arm immediately).
  s_dirty = false;
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
  s_dirty = false;  // committed — disarm Save until the next edit
  return true;
}

lv_obj_t* const* fade_widgets(int* out_n) {
  *out_n = s_fade_n;
  return s_fade;
}

}  // namespace espressopost::ui::preset_edit
