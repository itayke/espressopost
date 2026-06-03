#include "ui_connections.hpp"

#include "cloud.hpp"
#include "ui_theme.hpp"

#include <cstdio>

namespace espressopost::ui::connections_screen {
namespace {

// Status palette — semantic, not borrowed: green = good, amber = in progress,
// red = failed, gray = inactive/hint. Local so the screen owns its own meaning.
const lv_color_t kColorTitle = kColorText;
const lv_color_t kColorOk    = COLOR(0x5BB85B);
const lv_color_t kColorWarn  = COLOR(0xC88036);
const lv_color_t kColorErr   = COLOR(0xE07055);
const lv_color_t kColorHint  = COLOR(0x808080);
const lv_color_t kColorConnect = kColorText;
const lv_color_t kColorBack    = kColorText;

// ===== CONNECTIONS LAYOUT ===================================================
constexpr int32_t kTitleTopY          = 30;
constexpr int32_t kWifiLineTopY       = 122;
constexpr int32_t kSyncLineTopY       = 162;
constexpr int32_t kHintLineTopY       = 200;
constexpr int32_t kConnectBtnW        = 224;
constexpr int32_t kConnectBtnH        = 62;
constexpr int32_t kConnectCenterDY    = 44;   // below the round center
constexpr int32_t kBackBtnW           = 132;
constexpr int32_t kBackBtnH           = 56;
constexpr int32_t kBackBtnBottomInset = 16;
// ===========================================================================

lv_obj_t* s_title    = nullptr;
lv_obj_t* s_wifi_lbl = nullptr;
lv_obj_t* s_sync_lbl = nullptr;
lv_obj_t* s_hint_lbl = nullptr;
lv_obj_t* s_connect  = nullptr;
lv_obj_t* s_back     = nullptr;
lv_obj_t* s_fade[6]  = {};
int       s_fade_n   = 0;

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

lv_obj_t* build_status_line(lv_obj_t* group, int32_t top_y, const lv_font_t* font,
                            lv_color_t color) {
  lv_obj_t* l = lv_label_create(group);
  lv_obj_set_style_text_color(l, color, LV_PART_MAIN);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_width(l, kScreen - 80);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
  lv_label_set_text(l, "");
  lv_obj_align(l, LV_ALIGN_TOP_MID, 0, top_y);
  return l;
}

}  // namespace

lv_obj_t* build(lv_obj_t* scr, lv_event_cb_t on_back, lv_event_cb_t on_connect) {
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
  lv_label_set_text(s_title, "CONNECTIONS");
  lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, kTitleTopY);

  s_wifi_lbl = build_status_line(group, kWifiLineTopY, &lv_font_montserrat_24, kColorHint);
  s_sync_lbl = build_status_line(group, kSyncLineTopY, &lv_font_montserrat_14, kColorHint);
  s_hint_lbl = build_status_line(group, kHintLineTopY, &lv_font_montserrat_14, kColorHint);

  // Connect pill — kicks ESPTouch v2 provisioning via the injected handler.
  s_connect = lv_button_create(group);
  lv_obj_set_size(s_connect, kConnectBtnW, kConnectBtnH);
  lv_obj_set_style_radius(s_connect, kConnectBtnH / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_connect, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(s_connect, 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_connect, kColorConnect, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_connect, kPostBtnStroke, LV_PART_MAIN);
  lv_obj_set_style_border_opa(s_connect, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(s_connect, LV_ALIGN_CENTER, 0, kConnectCenterDY);
  lv_obj_set_ext_click_area(s_connect, kPostBtnExtClick);
  lv_obj_add_event_cb(s_connect, on_connect, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* connect_lbl = lv_label_create(s_connect);
  lv_obj_set_style_text_color(connect_lbl, kColorConnect, LV_PART_MAIN);
  lv_obj_set_style_text_font(connect_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(connect_lbl, "Connect Wi-Fi");
  lv_obj_center(connect_lbl);

  // Back pill — bottom-center, identical geometry to the other panels'.
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
  s_fade[s_fade_n++] = s_wifi_lbl;
  s_fade[s_fade_n++] = s_sync_lbl;
  s_fade[s_fade_n++] = s_hint_lbl;
  s_fade[s_fade_n++] = s_connect;
  s_fade[s_fade_n++] = s_back;

  refresh();
  return group;
}

void refresh() {
  if (s_wifi_lbl == nullptr) return;
  const cloud::Status st = cloud::status();

  // WiFi line — tinted by connection state.
  const char* wifi_txt = "Wi-Fi: unknown";
  lv_color_t  wifi_col = kColorHint;
  char wifi_buf[40];
  switch (st.wifi) {
    case cloud::WifiState::Disabled:
      wifi_txt = "Wi-Fi: not set up"; wifi_col = kColorHint; break;
    case cloud::WifiState::Provisioning:
      wifi_txt = "Wi-Fi: listening..."; wifi_col = kColorWarn; break;
    case cloud::WifiState::Connecting:
      wifi_txt = "Wi-Fi: connecting..."; wifi_col = kColorWarn; break;
    case cloud::WifiState::Connected:
      std::snprintf(wifi_buf, sizeof(wifi_buf), "Wi-Fi: connected (%d dBm)", st.rssi_dbm);
      wifi_txt = wifi_buf; wifi_col = kColorOk; break;
    case cloud::WifiState::Failed:
      wifi_txt = "Wi-Fi: failed"; wifi_col = kColorErr; break;
  }
  lv_label_set_text(s_wifi_lbl, wifi_txt);
  lv_obj_set_style_text_color(s_wifi_lbl, wifi_col, LV_PART_MAIN);

  // Sync line.
  char sync_buf[48];
  if (!st.configured) {
    lv_label_set_text(s_sync_lbl, "Cloud: endpoint not set");
  } else if (st.pending_count == 0) {
    std::snprintf(sync_buf, sizeof(sync_buf), "Cloud: up to date (%u synced)",
                  static_cast<unsigned>(st.synced_count));
    lv_label_set_text(s_sync_lbl, sync_buf);
  } else {
    std::snprintf(sync_buf, sizeof(sync_buf), "Cloud: %u of %u uploaded",
                  static_cast<unsigned>(st.synced_count),
                  static_cast<unsigned>(st.synced_count + st.pending_count));
    lv_label_set_text(s_sync_lbl, sync_buf);
  }

  // Hint line — what to do next, by state.
  const char* hint = "";
  if (st.wifi == cloud::WifiState::Provisioning) {
    hint = "Open the EspTouch app on your phone";
  } else if (st.wifi == cloud::WifiState::Disabled ||
             st.wifi == cloud::WifiState::Failed) {
    hint = "Tap Connect, then use the EspTouch app";
  } else if (st.wifi == cloud::WifiState::Connected && !st.configured) {
    hint = "Set endpoint over serial: cloud set-url / set-token";
  }
  lv_label_set_text(s_hint_lbl, hint);
}

lv_obj_t* const* fade_widgets(int* out_n) {
  *out_n = s_fade_n;
  return s_fade;
}

}  // namespace espressopost::ui::connections_screen
