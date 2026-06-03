#include "ui_menu.hpp"

#include "ui_theme.hpp"

namespace espressopost::ui::menu_screen {
namespace {

// Neutral palette — the menu reads as plain navigation chrome, so title /
// entries / Back all borrow the base text color (matching the Presets Back pill).
const lv_color_t kColorTitle = kColorText;
const lv_color_t kColorEntry = kColorText;
const lv_color_t kColorBack  = kColorText;

// ===== MENU LAYOUT ==========================================================
// Title up top, two entry pills stacked just above center, Back on the bottom
// arc — same Back geometry as the Presets screen so it doesn't jump between
// panels. Entry pills are centered horizontally; widen kEntryW or add rows here
// as more menu items land (the fade-set array below sizes for a few spares).
constexpr int32_t kTitleTopY          = 30;
constexpr int32_t kEntryW             = 264;
constexpr int32_t kEntryH             = 66;
constexpr int32_t kEntryGap           = 22;
constexpr int32_t kEntryFirstTopY     = 138;  // top inset of the first entry pill
constexpr int32_t kBackBtnW           = 132;
constexpr int32_t kBackBtnH           = 56;
constexpr int32_t kBackBtnBottomInset = 16;
// ===========================================================================

lv_obj_t* s_title    = nullptr;
lv_obj_t* s_presets  = nullptr;
lv_obj_t* s_conn     = nullptr;
lv_obj_t* s_back     = nullptr;
lv_obj_t* s_fade[4]  = {};
int       s_fade_n   = 0;

// Left chevron "<" for the Back pill — mirrors the one in ui_presets so the Back
// pill looks identical across panels.
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

// One outlined entry pill: centered label, right-chevron affordance.
lv_obj_t* build_entry(lv_obj_t* group, const char* text, int32_t top_y,
                      lv_event_cb_t on_tap) {
  lv_obj_t* b = lv_button_create(group);
  lv_obj_set_size(b, kEntryW, kEntryH);
  lv_obj_set_style_radius(b, kEntryH / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(b, kColorEntry, LV_PART_MAIN);
  lv_obj_set_style_border_width(b, kPostBtnStroke, LV_PART_MAIN);
  lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(b, LV_ALIGN_TOP_MID, 0, top_y);
  lv_obj_set_ext_click_area(b, kPostBtnExtClick);
  lv_obj_add_event_cb(b, on_tap, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* lbl = lv_label_create(b);
  lv_obj_set_style_text_color(lbl, kColorEntry, LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(lbl, text);
  lv_obj_center(lbl);
  return b;
}

}  // namespace

lv_obj_t* build(lv_obj_t* scr, lv_event_cb_t on_back,
                lv_event_cb_t on_presets, lv_event_cb_t on_connections) {
  lv_obj_t* group = lv_obj_create(scr);
  lv_obj_set_size(group, kScreen, kScreen);
  lv_obj_set_pos(group, 0, 0);
  lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(group, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(group, 0, LV_PART_MAIN);
  lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(group, LV_OBJ_FLAG_CLICKABLE);

  s_title = lv_label_create(group);
  lv_obj_set_style_text_color(s_title, kColorTitle, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_title, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(s_title, "MENU");
  lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, kTitleTopY);

  s_presets = build_entry(group, "Presets", kEntryFirstTopY, on_presets);
  s_conn    = build_entry(group, "Connections",
                          kEntryFirstTopY + kEntryH + kEntryGap, on_connections);

  // Back pill — bottom-center, identical geometry to the Presets screen's.
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

  s_fade_n = 0;
  s_fade[s_fade_n++] = s_title;
  s_fade[s_fade_n++] = s_presets;
  s_fade[s_fade_n++] = s_conn;
  s_fade[s_fade_n++] = s_back;

  return group;
}

lv_obj_t* const* fade_widgets(int* out_n) {
  *out_n = s_fade_n;
  return s_fade;
}

}  // namespace espressopost::ui::menu_screen
