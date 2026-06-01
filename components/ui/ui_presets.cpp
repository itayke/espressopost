#include "ui_presets.hpp"

#include "presets.hpp"
#include "ui_preset_readout.hpp"
#include "ui_theme.hpp"

namespace espressopost::ui::presets_screen {

// Menu pill accent — its own const so it can drift from the POST amber. Defined
// at namespace scope (not in the anon block below) because it's exported.
const lv_color_t kColorMenu = COLOR(0xC88036);

namespace {

// Remaining palette — internal to the screen. kColorBack tints the Back pill
// (chevron + label); kColorSlotEmpty outlines a slot with no preset; the title
// borrows the base text color.
const lv_color_t kColorBack         = kColorText;
const lv_color_t kColorSlotEmpty    = kColorMuted3;
const lv_color_t kColorPresetsTitle = kColorText;

// ===== MENU / BACK GLYPHS ===================================================
// Custom-drawn (house style — matches the star / arrow / climate-icon painters)
// so they tint + scale exactly. Hamburger = three stacked rounded bars in
// kColorMenu; back chevron = a left-pointing "<" polyline.
constexpr int32_t kHamburgerW    = 26;   // bar length
constexpr int32_t kHamburgerH    = 18;   // glyph box height
constexpr int32_t kHamburgerBarH = 3;    // each bar thickness
constexpr int32_t kHamburgerGap  = 6;    // center-to-center spacing of the bars

void draw_hamburger_event(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  if (layer == nullptr) return;
  auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  const int32_t cx = coords.x1 + lv_area_get_width(&coords)  / 2;
  const int32_t cy = coords.y1 + lv_area_get_height(&coords) / 2;

  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = kColorMenu;
  d.bg_opa   = LV_OPA_COVER;
  d.radius   = kHamburgerBarH / 2;

  const int32_t half_w = kHamburgerW / 2;
  const int32_t half_h = kHamburgerBarH / 2;
  for (int row = -1; row <= 1; ++row) {
    const int32_t yc = cy + row * kHamburgerGap;
    lv_area_t a = {cx - half_w, yc - half_h, cx + half_w, yc + half_h};
    lv_draw_rect(layer, &d, &a);
  }
}

// Left chevron "<" for the Back pill — a single rounded polyline.
lv_obj_t* build_back_chevron(lv_obj_t* parent, lv_color_t color) {
  constexpr int32_t kW = 11, kH = 18, kStroke = 3;
  static lv_point_precise_t pts[] = {
      {kW - 1, 1},
      {1,      kH / 2.0f},
      {kW - 1, kH - 1},
  };
  lv_obj_t* c = lv_obj_create(parent);
  lv_obj_set_size(c, kW, kH);
  lv_obj_set_style_bg_opa(c, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(c, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(c, 0, LV_PART_MAIN);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* line = lv_line_create(c);
  lv_line_set_points(line, pts, 3);
  lv_obj_set_style_line_color(line, color, LV_PART_MAIN);
  lv_obj_set_style_line_width(line, kStroke, LV_PART_MAIN);
  lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
  return c;
}
// ===========================================================================

// ===== PRESETS TUNING =======================================================
// Presets screen — a 3×3 slot grid + a Back pill, all inside the round panel.
// The grid bounding box is sized so its outer corners stay within the circle:
// at kGridCell=92 / kGridGap=12 the 300×300 box's corners sit ~212 px from
// center (< the 233 radius). kGridTopY pushes the grid a touch above center to
// open room for the Back pill on the bottom arc. Retune cell/gap/top together
// if the grid grows.
constexpr int32_t kPresetsTitleTopY = 30;   // "PRESETS" header top inset

constexpr int32_t kGridCols   = 3;
constexpr int32_t kGridCell   = 92;
constexpr int32_t kGridGap    = 12;
constexpr int32_t kGridTopY   = 78;
constexpr int32_t kGridLeftX  =
    (kScreen - (kGridCols * kGridCell + (kGridCols - 1) * kGridGap)) / 2;
constexpr int32_t kSlotRadius = 14;
constexpr int32_t kSlotBorder = 2;
constexpr int32_t kSlotPad    = 4;

constexpr int32_t kBackBtnW           = 132;
constexpr int32_t kBackBtnH           = 56;
constexpr int32_t kBackBtnBottomInset = 16;
// ===========================================================================

// Screen state. The group handle itself is returned to (and owned by) ui_report;
// these are the inner widgets this module needs to refresh / fade.
lv_obj_t*     s_title = nullptr;
lv_obj_t*     s_slot[presets::kMaxPresets] = {};   // the 9 bordered slot rects
PresetReadout s_grid[presets::kMaxPresets] = {};   // each slot's readout
lv_obj_t*     s_back  = nullptr;
lv_obj_t*     s_fade[presets::kMaxPresets + 2] = {};  // title + slots + Back
int           s_fade_n = 0;

}  // namespace

lv_obj_t* build_menu_glyph(lv_obj_t* parent) {
  lv_obj_t* glyph = lv_obj_create(parent);
  lv_obj_set_size(glyph, kHamburgerW, kHamburgerH);
  lv_obj_set_style_bg_opa(glyph, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(glyph, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(glyph, 0, LV_PART_MAIN);
  lv_obj_clear_flag(glyph, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(glyph, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(glyph, draw_hamburger_event, LV_EVENT_DRAW_MAIN, nullptr);
  return glyph;
}

lv_obj_t* build(lv_obj_t* scr, lv_event_cb_t on_back) {
  lv_obj_t* group = lv_obj_create(scr);
  lv_obj_set_size(group, kScreen, kScreen);
  lv_obj_set_pos(group, 0, 0);
  lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(group, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(group, 0, LV_PART_MAIN);
  lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(group, LV_OBJ_FLAG_CLICKABLE);

  // Screen title, centered above the grid.
  s_title = lv_label_create(group);
  lv_obj_set_style_text_color(s_title, kColorPresetsTitle, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_title, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(s_title, "PRESETS");
  lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, kPresetsTitleTopY);

  // 3×3 slot grid. Each slot is a bordered rounded rect with a compact readout
  // inside; active/empty styling + text are pushed by refresh() each time the
  // screen opens, so they always reflect current preset data. Display-only for
  // now (no per-slot click) — slot interactions are a later change.
  for (uint8_t i = 0; i < presets::kMaxPresets; ++i) {
    const int32_t row = i / kGridCols;
    const int32_t col = i % kGridCols;
    lv_obj_t* slot = lv_obj_create(group);
    lv_obj_set_size(slot, kGridCell, kGridCell);
    lv_obj_set_pos(slot, kGridLeftX + col * (kGridCell + kGridGap),
                         kGridTopY  + row * (kGridCell + kGridGap));
    lv_obj_set_style_radius(slot, kSlotRadius, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_color(slot, kColorSlotEmpty, LV_PART_MAIN);
    lv_obj_set_style_border_width(slot, kSlotBorder, LV_PART_MAIN);
    lv_obj_set_style_border_opa(slot, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(slot, kSlotPad, LV_PART_MAIN);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);
    s_slot[i] = slot;
    s_grid[i] = build_preset_readout_grid(slot);
    lv_obj_center(s_grid[i].root);
  }

  // Back pill — bottom-center, outlined like the action pills, with a left
  // chevron + "Back" label. Click handler is injected by ui_report (returns to
  // Idle via the reverse section fade).
  s_back = lv_button_create(group);
  lv_obj_set_size(s_back, kBackBtnW, kBackBtnH);
  lv_obj_set_style_radius(s_back, kBackBtnH / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_back, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_back, 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_back, kColorBack, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_back, kPostBtnStroke, LV_PART_MAIN);
  lv_obj_set_style_border_opa(s_back, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(s_back, LV_ALIGN_BOTTOM_MID, 0, -kBackBtnBottomInset);
  lv_obj_set_ext_click_area(s_back, kPostBtnExtClick);
  lv_obj_add_event_cb(s_back, on_back, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* back_row = lv_obj_create(s_back);
  lv_obj_set_size(back_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(back_row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(back_row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(back_row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(back_row, 8, LV_PART_MAIN);
  lv_obj_set_layout(back_row, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(back_row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(back_row, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(back_row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(back_row, LV_OBJ_FLAG_SCROLLABLE);
  build_back_chevron(back_row, kColorBack);
  lv_obj_t* back_lbl = lv_label_create(back_row);
  lv_obj_set_style_text_color(back_lbl, kColorBack, LV_PART_MAIN);
  lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(back_lbl, "Back");
  lv_obj_center(back_row);

  // Fade set the swap engine iterates: title + slots + Back.
  s_fade_n = 0;
  s_fade[s_fade_n++] = s_title;
  for (uint8_t i = 0; i < presets::kMaxPresets; ++i) s_fade[s_fade_n++] = s_slot[i];
  s_fade[s_fade_n++] = s_back;

  return group;
}

// Repopulate the grid from current preset data — active slots show their readout
// (text + color) and an outline in the preset color; empty slots hide the
// readout and outline in the muted empty color. Run at the swap midpoint so the
// grid is correct before it fades in.
void refresh() {
  for (uint8_t i = 0; i < presets::kMaxPresets; ++i) {
    if (presets::is_active(i)) {
      const auto p = presets::get(i);
      apply_readout(s_grid[i], p, i);
      lv_obj_remove_flag(s_grid[i].root, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_border_color(s_slot[i], lv_color_hex(p.color),
                                    LV_PART_MAIN);
    } else {
      lv_obj_add_flag(s_grid[i].root, LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_border_color(s_slot[i], kColorSlotEmpty, LV_PART_MAIN);
    }
  }
}

lv_obj_t* const* fade_widgets(int* out_n) {
  *out_n = s_fade_n;
  return s_fade;
}

}  // namespace espressopost::ui::presets_screen
