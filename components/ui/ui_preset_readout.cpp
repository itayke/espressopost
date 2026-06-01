#include "ui_preset_readout.hpp"

#include <cstdio>
#include <initializer_list>

#include "ui_theme.hpp"  // kColorText

namespace espressopost::ui {
namespace {

// Arrow widget for the bottom row of the preset readout. Two strokes:
// a horizontal shaft and a V-shape head opening to the right. Sized to
// vertically pair with MS14 text — y-offset can be nudged later if the
// baseline disagrees. Color is passed in so the arrow re-tints with text.
lv_obj_t* build_preset_arrow(lv_obj_t* parent, lv_color_t color) {
  constexpr int32_t kArrowW = 12;
  constexpr int32_t kArrowH = 10;
  constexpr int32_t kStroke = 1;
  constexpr float   kMidY   = kArrowH / 2.0f;  // float context — LV_USE_FLOAT is on
  // Static so both readouts can share the same vertex buffers without
  // duplicating arrays — lv_line just stores the pointer.
  static lv_point_precise_t shaft_pts[] = {
      {0,           kMidY},
      {kArrowW - 4, kMidY},
  };
  static lv_point_precise_t head_pts[] = {
      {kArrowW - 4, 2},
      {kArrowW - 1, kMidY},
      {kArrowW - 4, kArrowH - 2},
  };

  lv_obj_t* container = lv_obj_create(parent);
  lv_obj_set_size(container, kArrowW, kArrowH);
  lv_obj_set_style_bg_opa(container, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(container, 0, LV_PART_MAIN);
  lv_obj_clear_flag(container, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(container, LV_OBJ_FLAG_SCROLLABLE);

  auto stroke = [&](const lv_point_precise_t* pts, uint32_t n) {
    lv_obj_t* line = lv_line_create(container);
    lv_line_set_points(line, pts, n);
    lv_obj_set_style_line_color(line, color, LV_PART_MAIN);
    lv_obj_set_style_line_width(line, kStroke, LV_PART_MAIN);
    lv_obj_set_style_line_rounded(line, true, LV_PART_MAIN);
  };
  stroke(shaft_pts, 2);
  stroke(head_pts,  3);
  return container;
}

}  // namespace

// Build the two-line preset readout consumed by both the idle (tappable)
// preset button and the post-mode (read-only) surface. Top line is the
// 1-based slot index ("PRESET N", MS24); bottom line is a flex row
// [dose | arrow | yield | time] in MS14 — the user reads the active
// preset and target values at a glance without leaving the screen. Both
// modes share the same readout layout so cycling presets in idle
// pre-stages exactly what post mode will display on entry. Names live in
// the Preset struct for wire-format continuity but are no longer surfaced
// — slots are identified by index.
PresetReadout build_preset_readout(lv_obj_t* parent) {
  PresetReadout r = {};

  r.root = lv_obj_create(parent);
  lv_obj_set_size(r.root, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(r.root, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(r.root, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(r.root, 0, LV_PART_MAIN);
  lv_obj_set_layout(r.root, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(r.root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(r.root, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(r.root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(r.root, LV_OBJ_FLAG_SCROLLABLE);

  r.top = lv_label_create(r.root);
  lv_obj_set_style_text_color(r.top, kColorText, LV_PART_MAIN);
  lv_obj_set_style_text_font(r.top, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_style_text_align(r.top, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

  lv_obj_t* row = lv_obj_create(r.root);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
  lv_obj_set_layout(row, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  auto small_label = [&]() {
    lv_obj_t* l = lv_label_create(row);
    lv_obj_set_style_text_color(l, kColorText, LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
    return l;
  };
  r.dose  = small_label();
  r.arrow = build_preset_arrow(row, kColorText);
  r.yield = small_label();
  r.time  = small_label();
  return r;
}

// Compact readout for a Presets-screen grid slot. Same data + same PresetReadout
// fields as the center-line readout (so apply_readout drives both), but laid out
// to fit a small square cell: everything at MS14 and the time pushed to its own
// third line ("PRESET N" / "Xg → Yg" / "Ys") instead of the single wide row the
// center line uses. All labels seed to kColorText; apply_readout owns color.
PresetReadout build_preset_readout_grid(lv_obj_t* parent) {
  PresetReadout r = {};

  r.root = lv_obj_create(parent);
  lv_obj_set_size(r.root, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(r.root, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(r.root, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(r.root, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(r.root, 2, LV_PART_MAIN);
  lv_obj_set_layout(r.root, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(r.root, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(r.root, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(r.root, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(r.root, LV_OBJ_FLAG_SCROLLABLE);

  auto label = [&](lv_obj_t* p) {
    lv_obj_t* l = lv_label_create(p);
    lv_obj_set_style_text_color(l, kColorText, LV_PART_MAIN);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_14, LV_PART_MAIN);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    return l;
  };

  r.top = label(r.root);

  lv_obj_t* row = lv_obj_create(r.root);
  lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
  lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(row, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(row, 4, LV_PART_MAIN);
  lv_obj_set_layout(row, LV_LAYOUT_FLEX);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);

  r.dose  = label(row);
  r.arrow = build_preset_arrow(row, kColorText);
  r.yield = label(row);
  r.time  = label(r.root);
  return r;
}

// Push one preset's values AND its accent color into a single readout. Shared
// by the idle/post selected-preset readouts and the Presets-screen grid slots —
// `slot` is the 0-based id (rendered 1-based as "PRESET N"), `p` its data. The
// color tints every label plus both arrow strokes so the whole readout reads in
// the preset's hue.
void apply_readout(PresetReadout& r, const presets::Preset& p, uint8_t slot) {
  if (r.root == nullptr) return;
  char top_buf[16], dose_buf[8], yield_buf[8], time_buf[8];
  std::snprintf(top_buf,   sizeof(top_buf),   "PRESET %u",
                static_cast<unsigned>(slot + 1));
  std::snprintf(dose_buf,  sizeof(dose_buf),  "%ug",
                static_cast<unsigned>(p.dose_g));
  std::snprintf(yield_buf, sizeof(yield_buf), "%ug",
                static_cast<unsigned>(p.yield_g));
  std::snprintf(time_buf,  sizeof(time_buf),  " %us",
                static_cast<unsigned>(p.target_time_s));
  lv_label_set_text(r.top,   top_buf);
  lv_label_set_text(r.dose,  dose_buf);
  lv_label_set_text(r.yield, yield_buf);
  lv_label_set_text(r.time,  time_buf);

  const lv_color_t c = lv_color_hex(p.color);
  for (lv_obj_t* lbl : {r.top, r.dose, r.yield, r.time}) {
    lv_obj_set_style_text_color(lbl, c, LV_PART_MAIN);
  }
  // The arrow is a container of lv_line strokes — tint each child.
  if (r.arrow != nullptr) {
    const uint32_t n = lv_obj_get_child_count(r.arrow);
    for (uint32_t i = 0; i < n; ++i) {
      lv_obj_set_style_line_color(lv_obj_get_child(r.arrow, i), c, LV_PART_MAIN);
    }
  }
}

}  // namespace espressopost::ui
