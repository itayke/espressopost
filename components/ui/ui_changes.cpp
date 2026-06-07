#include "ui_changes.hpp"

#include "ui_theme.hpp"

#include "calibration.hpp"
#include "rtc.hpp"

#include <cstdint>  // uintptr_t
#include <cstdio>   // snprintf
#include <cstdlib>  // setenv
#include <ctime>    // localtime_r, strftime, tzset

namespace espressopost::ui::changes_screen {
namespace {

// Neutral chrome like the other Menu panels; the status line at the top tints
// semantically (green = just saved, gray = on-record/empty, amber = blocked) so
// the one dynamic line carries all the state colour. Each is its own const so it
// can be retuned in isolation (house rule), even where several seed to the same
// base tier today.
const lv_color_t kColorTitle      = kColorText;
const lv_color_t kColorStatusOk   = COLOR(0x5BB85B);  // just-saved confirmation
const lv_color_t kColorStatusInfo = kColorMuted;       // on-record / nothing-yet
const lv_color_t kColorStatusWarn = COLOR(0xC88036);   // clock-not-set / list-full
const lv_color_t kColorTypeOn   = COLOR(0xC88036);  // selected reason pill (fill + on-border), matches taste pills
const lv_color_t kColorTypeOff  = kColorMuted;      // unselected reason pill (outline + label)
const lv_color_t kColorStepDisc = COLOR(0xC88036);  // − / + discs (amber, matches the value steppers)
const lv_color_t kColorDate     = kColorText;       // the selected-date readout
const lv_color_t kColorCaption  = kColorMuted;      // section prompts
const lv_color_t kColorSaveOn   = kColorText;       // Save enabled (a reason is picked)
const lv_color_t kColorSaveOff  = kColorMuted;      // Save disabled (nothing to commit)
const lv_color_t kColorUndo     = COLOR(0xE07055);  // cancel-red, matching the Post ✕ pill
const lv_color_t kColorBack     = kColorText;

// ===== EVENTS LAYOUT ========================================================
// Title; the on-record status line under it; a one-line prompt over a row of
// three single-select reason pills (Grinder / Beans / Machine — radio, no commit
// on tap); an "EVENT DATE" label over a date stepper; a Save pill (gated on a
// pick) centered alone, joined by a cancel-red Undo as a centered pair after a
// save; Back on the bottom arc (same geometry as the other panels).
constexpr int32_t kTitleTopY      = 30;
constexpr int32_t kStatusTopY     = 66;    // "Last event: …" line, under the title
constexpr int32_t kCaptionTopY    = 102;    // prompt, above the reason pills
constexpr int32_t kTypeRowTopY    = 132;   // the three reason pills
constexpr int32_t kTypePillH      = (kPostBtnH * 4) / 5;  // same height as the Post taste pills
constexpr int32_t kTypePillStroke = 2;     // thin outline, matching the taste pills
constexpr int32_t kTypeTextPadX   = 27;    // total chrome around pill text (auto-width)
constexpr int32_t kTypeRowGap     = 8;     // gap between adjacent pills
constexpr int32_t kTypeExtClick   = 10;    // hit-area pad per side
constexpr int32_t kDateHdrTopY    = 210;   // "EVENT DATE" caption above the selector
constexpr int32_t kStepperTopY    = 229;   // top of the − / + discs (selector sits below the pills)
constexpr int32_t kStepBtnSize    = 56;
constexpr int32_t kStepperDX      = 128;   // − / + x-offset from center (room for a date between them)
constexpr int32_t kDateLblTopY    = 247;   // date readout, vertically centered in the discs
// Save + Undo sit side by side — tall but thin so both fit the arc. Save is
// centered alone until a save; Undo (cancel-red) then joins it as a centered
// pair. A comfortable gap sits under the row before Back.
constexpr int32_t kActionRowTopY  = 310;
constexpr int32_t kActionBtnH     = 66;
constexpr int32_t kSaveW          = 150;
constexpr int32_t kUndoW          = 150;
constexpr int32_t kActionGap      = 16;    // gap between Save and Undo when paired
constexpr int32_t kBackBtnW       = 132;
constexpr int32_t kBackBtnH       = 56;
constexpr int32_t kBackBtnBottomInset = 16;

// How far back the date stepper can reach. A maintenance change is logged at
// most a few weeks late in practice; the cap just bounds the backward stepping.
constexpr int kMaxDaysAgo = 120;
constexpr uint32_t kSecondsPerDay = 86400;
// ===========================================================================

// Wall-clock zone for rendering a boundary's date (the RTC stores UTC). Matches
// the Menu clock's zone — change both to relocate the displayed dates.
constexpr const char* kPosixTz = "EST5EDT,M3.2.0,M11.1.0";

lv_obj_t* s_group   = nullptr;
lv_obj_t* s_title   = nullptr;
lv_obj_t* s_status  = nullptr;   // "Last event: <type> <date>" — also the post-save confirmation
lv_obj_t* s_caption = nullptr;
lv_obj_t* s_date_hdr = nullptr;  // "EVENT DATE" caption above the selector
lv_obj_t* s_minus   = nullptr;
lv_obj_t* s_plus    = nullptr;
lv_obj_t* s_date    = nullptr;   // selected boundary date, e.g. "31 May 2026"
lv_obj_t* s_save    = nullptr;
lv_obj_t* s_save_lbl = nullptr;
lv_obj_t* s_undo    = nullptr;   // appears only right after a successful save
lv_obj_t* s_back    = nullptr;
lv_obj_t* s_fade[14] = {};
int       s_fade_n   = 0;

// The three reason pills — single-select radio. label == displayed type.
struct TypePill { calibration::Kind kind; const char* label; lv_obj_t* btn; lv_obj_t* lbl; };
TypePill s_pills[3] = {
    {calibration::Kind::Grinder, "Grinder", nullptr, nullptr},
    {calibration::Kind::Beans,   "Beans",   nullptr, nullptr},
    {calibration::Kind::Machine, "Machine", nullptr, nullptr},
};

bool              s_has_sel = false;                  // is a reason currently picked?
calibration::Kind s_sel     = calibration::Kind::Grinder;

int s_days_ago = 0;  // how far back the dialed date sits from today (0 = today)

// The boundary the last save added, so Undo can pull that exact one back out
// even if it isn't the newest by timestamp (a backdated entry sorts mid-list).
bool              s_have_last = false;
uint32_t          s_last_rtc  = 0;
calibration::Kind s_last_kind = calibration::Kind::Grinder;

const char* kind_label(calibration::Kind k) {
  switch (k) {
    case calibration::Kind::Grinder: return "Grinder";
    case calibration::Kind::Beans:   return "Beans";
    case calibration::Kind::Machine: return "Machine";
  }
  return "?";
}

// Repaint the radio pills: the selected one fills amber (bg-coloured text), the
// rest read as quiet muted outlines — same on/off treatment as the taste pills.
void refresh_type_pills() {
  for (auto& p : s_pills) {
    if (p.btn == nullptr) continue;
    const bool on = s_has_sel && p.kind == s_sel;
    lv_obj_set_style_bg_opa(p.btn, on ? LV_OPA_COVER : LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_bg_color(p.btn, kColorTypeOn, LV_PART_MAIN);
    lv_obj_set_style_border_color(p.btn, on ? kColorTypeOn : kColorTypeOff, LV_PART_MAIN);
    if (p.lbl != nullptr)
      lv_obj_set_style_text_color(p.lbl, on ? kColorBg : kColorTypeOff, LV_PART_MAIN);
  }
}

// Save unlocks only once a reason is picked; recolor to advertise the gate.
void refresh_save_enabled() {
  if (s_save == nullptr) return;
  const lv_color_t c = s_has_sel ? kColorSaveOn : kColorSaveOff;
  lv_obj_set_style_border_color(s_save, c, LV_PART_MAIN);
  if (s_save_lbl != nullptr) lv_obj_set_style_text_color(s_save_lbl, c, LV_PART_MAIN);
}

// Place the Save pill — centered alone, or paired with Undo (centered as a
// two-up) once a save has happened and Undo is showing. Save is always visible;
// only Undo's visibility toggles.
void layout_actions() {
  if (s_save == nullptr) return;
  const bool undo_on = s_undo != nullptr && !lv_obj_has_flag(s_undo, LV_OBJ_FLAG_HIDDEN);
  if (undo_on) {
    const int32_t pair = kSaveW + kActionGap + kUndoW;
    lv_obj_align(s_save, LV_ALIGN_TOP_MID, -(pair / 2 - kSaveW / 2), kActionRowTopY);
    lv_obj_align(s_undo, LV_ALIGN_TOP_MID, +(pair / 2 - kUndoW / 2), kActionRowTopY);
  } else {
    lv_obj_align(s_save, LV_ALIGN_TOP_MID, 0, kActionRowTopY);
  }
}

// The wall-clock second for the date currently dialed in (now − N days), or 0 if
// the RTC has no time to anchor it.
uint32_t selected_rtc() {
  const uint32_t now = rtc::epoch_s();
  if (now == 0) return 0;
  return now - static_cast<uint32_t>(s_days_ago) * kSecondsPerDay;
}

// Render a boundary instant as "31 May 2026", localized via the screen's TZ.
// "?" for an unanchored (rtc==0) instant.
void format_date(uint32_t rtc, char* buf, size_t n) {
  if (rtc == 0) { std::snprintf(buf, n, "?"); return; }
  const time_t t = static_cast<time_t>(rtc);
  struct tm lt;
  localtime_r(&t, &lt);
  char mon[8];
  strftime(mon, sizeof(mon), "%b", &lt);
  std::snprintf(buf, n, "%d %s %d", lt.tm_mday, mon, 1900 + lt.tm_year);
}

void refresh_date_label() {
  const uint32_t when = selected_rtc();
  if (when == 0) { lv_label_set_text(s_date, "clock not set"); return; }
  char buf[24];
  format_date(when, buf, sizeof(buf));
  lv_label_set_text(s_date, buf);
}

// Paint the top status line from the store: the newest boundary as
// "Last event: <type> <date>", or the empty-state hint. Also the browse-state
// reset, so it clears s_have_last and swaps the action slot back to Save.
void show_status() {
  s_have_last = false;
  if (s_undo != nullptr) lv_obj_add_flag(s_undo, LV_OBJ_FLAG_HIDDEN);

  calibration::Boundary b[calibration::kMaxBoundaries];
  const size_t n = calibration::list(b, calibration::kMaxBoundaries);
  if (n == 0) {
    lv_label_set_text(s_status, "No events logged yet");
    lv_obj_set_style_text_color(s_status, kColorStatusInfo, LV_PART_MAIN);
    return;
  }
  // list() is sorted ascending by time, so the last entry is the newest.
  const calibration::Boundary& newest = b[n - 1];
  char date[24];
  format_date(newest.rtc_epoch_s, date, sizeof(date));
  char buf[56];
  std::snprintf(buf, sizeof(buf), "Last event: %s %s",
                kind_label(static_cast<calibration::Kind>(newest.kind)), date);
  lv_label_set_text(s_status, buf);
  lv_obj_set_style_text_color(s_status, kColorStatusInfo, LV_PART_MAIN);
}

// Commit a boundary for the picked reason at the dialed-in date. The top status
// line doubles as the confirmation (it now reflects the just-saved entry), tinted
// green; on success Undo is revealed to recover a mistap.
void save_selection() {
  if (!s_has_sel) return;
  const uint32_t when = selected_rtc();
  if (when == 0) {
    // No wall clock to anchor a dated boundary — refuse rather than log a bogus
    // epoch-0 timestamp that can't be placed against the shot log.
    lv_label_set_text(s_status, "Clock not set, can't log a date");
    lv_obj_set_style_text_color(s_status, kColorStatusWarn, LV_PART_MAIN);
    return;
  }
  if (calibration::add(when, s_sel) != ESP_OK) {
    lv_label_set_text(s_status, "Can't save, list full");
    lv_obj_set_style_text_color(s_status, kColorStatusWarn, LV_PART_MAIN);
    return;
  }
  s_have_last = true;
  s_last_rtc  = when;
  s_last_kind = s_sel;

  char date[24];
  format_date(when, date, sizeof(date));
  char buf[56];
  std::snprintf(buf, sizeof(buf), "Last event: %s %s", kind_label(s_sel), date);
  lv_label_set_text(s_status, buf);
  lv_obj_set_style_text_color(s_status, kColorStatusOk, LV_PART_MAIN);

  // Saved: clear the radio (so Save greys out and a re-tap can't double-log) and
  // reveal Undo alongside Save as a centered pair.
  s_has_sel = false;
  refresh_type_pills();
  refresh_save_enabled();
  lv_obj_remove_flag(s_undo, LV_OBJ_FLAG_HIDDEN);
  layout_actions();
}

void on_minus(lv_event_t*) {  // step the date back in time
  if (s_days_ago < kMaxDaysAgo) { ++s_days_ago; refresh_date_label(); }
}
void on_plus(lv_event_t*) {    // step forward, clamped at today (no future date)
  if (s_days_ago > 0) { --s_days_ago; refresh_date_label(); }
}

// Pick a reason (radio). Selecting supersedes a just-saved entry, so its Undo
// drops away and Save re-arms.
void on_type_tap(lv_event_t* e) {
  s_sel = static_cast<calibration::Kind>(
      reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
  s_has_sel = true;
  // Picking supersedes a just-saved entry: drop its Undo, re-center Save.
  if (s_undo != nullptr) lv_obj_add_flag(s_undo, LV_OBJ_FLAG_HIDDEN);
  refresh_type_pills();
  refresh_save_enabled();
  layout_actions();
}

void on_save(lv_event_t*) { save_selection(); }  // no-ops while disabled (s_has_sel false)

// Pull the just-saved boundary back out (recover from a mistap / wrong date).
void on_undo(lv_event_t*) {
  if (!s_have_last) return;
  calibration::Boundary b[calibration::kMaxBoundaries];
  const size_t n = calibration::list(b, calibration::kMaxBoundaries);
  for (size_t i = 0; i < n; ++i) {
    if (b[i].rtc_epoch_s == s_last_rtc &&
        b[i].kind == static_cast<uint8_t>(s_last_kind)) {
      calibration::remove_at(i);
      break;
    }
  }
  show_status();           // repaints on-record + hides Undo
  refresh_save_enabled();  // selection is still none → Save stays disabled
  layout_actions();        // re-center Save now that Undo is gone
}

// Left chevron "<" for the Back pill — mirrors ui_menu / ui_connections so the
// Back pill looks identical across panels.
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

// A round outlined disc carrying a centered LV_SYMBOL glyph (−/+), same chrome as
// the value steppers' discs — the symbol font sits properly centered, where a
// bare "-" rides high on its text baseline.
lv_obj_t* build_step_btn(lv_obj_t* group, const char* glyph, int32_t dx,
                         lv_event_cb_t on_tap) {
  lv_obj_t* b = lv_button_create(group);
  lv_obj_set_size(b, kStepBtnSize, kStepBtnSize);
  lv_obj_set_style_radius(b, kStepBtnSize / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(b, kColorStepDisc, LV_PART_MAIN);
  lv_obj_set_style_border_width(b, kPostBtnStroke, LV_PART_MAIN);
  lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_align(b, LV_ALIGN_TOP_MID, dx, kStepperTopY);
  lv_obj_set_ext_click_area(b, kPostBtnExtClick);
  lv_obj_add_event_cb(b, on_tap, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* lbl = lv_label_create(b);
  lv_obj_set_style_text_color(lbl, kColorStepDisc, LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(lbl, glyph);
  lv_obj_center(lbl);
  return b;
}

// A bottom-row action pill (Save / Undo): outline only, label centered, color
// set by the caller per state. layout_actions() positions it in the row.
lv_obj_t* build_action_btn(lv_obj_t* group, const char* text, int32_t w,
                           lv_color_t color, lv_event_cb_t on_tap,
                           lv_obj_t** out_lbl) {
  lv_obj_t* b = lv_button_create(group);
  lv_obj_set_size(b, w, kActionBtnH);
  lv_obj_set_style_radius(b, kActionBtnH / 2, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
  lv_obj_set_style_border_color(b, color, LV_PART_MAIN);
  lv_obj_set_style_border_width(b, kPostBtnStroke, LV_PART_MAIN);
  lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_ext_click_area(b, kPostBtnExtClick);
  lv_obj_add_event_cb(b, on_tap, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* lbl = lv_label_create(b);
  lv_obj_set_style_text_color(lbl, color, LV_PART_MAIN);
  lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(lbl, text);
  lv_obj_center(lbl);
  if (out_lbl != nullptr) *out_lbl = lbl;
  return b;
}

}  // namespace

lv_obj_t* build(lv_obj_t* scr, lv_event_cb_t on_back) {
  lv_obj_t* group = lv_obj_create(scr);
  lv_obj_set_size(group, kScreen, kScreen);
  lv_obj_set_pos(group, 0, 0);
  lv_obj_set_style_bg_opa(group, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(group, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(group, 0, LV_PART_MAIN);
  lv_obj_clear_flag(group, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(group, LV_OBJ_FLAG_CLICKABLE);
  s_group = group;

  s_title = lv_label_create(group);
  lv_obj_set_style_text_color(s_title, kColorTitle, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_title, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(s_title, "EVENTS");
  lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, kTitleTopY);

  // Date rendering uses localtime — the TZ is process-global; the Menu screen
  // sets it too, but set it here so dates are right even if Changes is reached
  // before the Menu ever paints. Must precede show_status / refresh_date_label.
  setenv("TZ", kPosixTz, 1);
  tzset();

  // On-record status line, right under the title.
  s_status = lv_label_create(group);
  lv_obj_set_style_text_font(s_status, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_obj_set_width(s_status, kScreen - 80);
  lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(s_status, LV_ALIGN_TOP_MID, 0, kStatusTopY);

  // Prompt, above the reason pills.
  s_caption = lv_label_create(group);
  lv_obj_set_style_text_color(s_caption, kColorCaption, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_caption, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(s_caption, "EVENT TYPE");
  lv_obj_align(s_caption, LV_ALIGN_TOP_MID, 0, kCaptionTopY);

  // Three reason pills, single-select. Measure each label (Mont 24) up front so
  // the row can be centered with each pill auto-sized to its own text, just like
  // the Post taste pills.
  int32_t pill_w[3];
  {
    lv_obj_t* scratch = lv_label_create(group);
    lv_obj_set_style_text_font(scratch, &lv_font_montserrat_24, LV_PART_MAIN);
    for (int i = 0; i < 3; ++i) {
      lv_label_set_text(scratch, s_pills[i].label);
      lv_obj_update_layout(scratch);
      pill_w[i] = lv_obj_get_width(scratch) + kTypeTextPadX;
    }
    lv_obj_delete(scratch);
  }
  const int32_t row_w = pill_w[0] + pill_w[1] + pill_w[2] + 2 * kTypeRowGap;
  int32_t px = (kScreen - row_w) / 2;
  for (int i = 0; i < 3; ++i) {
    lv_obj_t* b = lv_button_create(group);
    lv_obj_set_size(b, pill_w[i], kTypePillH);
    lv_obj_set_style_radius(b, kTypePillH / 2, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(b, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(b, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(b, kTypePillStroke, LV_PART_MAIN);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_pos(b, px, kTypeRowTopY);
    px += pill_w[i] + kTypeRowGap;
    lv_obj_set_ext_click_area(b, kTypeExtClick);
    lv_obj_add_event_cb(b, on_type_tap, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(
                            static_cast<uintptr_t>(s_pills[i].kind)));
    lv_obj_t* lbl = lv_label_create(b);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, LV_PART_MAIN);
    lv_label_set_text(lbl, s_pills[i].label);
    lv_obj_center(lbl);
    s_pills[i].btn = b;
    s_pills[i].lbl = lbl;
  }

  // Date selector lives below the pills, under its own caption.
  s_date_hdr = lv_label_create(group);
  lv_obj_set_style_text_color(s_date_hdr, kColorCaption, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_date_hdr, &lv_font_montserrat_14, LV_PART_MAIN);
  lv_label_set_text(s_date_hdr, "EVENT DATE");
  lv_obj_align(s_date_hdr, LV_ALIGN_TOP_MID, 0, kDateHdrTopY);

  // Date stepper: −  31 May 2026  +   (− steps back in time, + forward to today)
  s_minus = build_step_btn(group, LV_SYMBOL_MINUS, -kStepperDX, on_minus);
  s_plus  = build_step_btn(group, LV_SYMBOL_PLUS, kStepperDX, on_plus);
  s_date  = lv_label_create(group);
  lv_obj_set_style_text_color(s_date, kColorDate, LV_PART_MAIN);
  lv_obj_set_style_text_font(s_date, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_obj_set_width(s_date, 2 * kStepperDX - kStepBtnSize);
  lv_obj_set_style_text_align(s_date, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_align(s_date, LV_ALIGN_TOP_MID, 0, kDateLblTopY);
  refresh_date_label();

  // Save (gated by selection) + Undo (revealed only after a save), side by side.
  // layout_actions() positions them; Undo starts hidden so Save sits centered.
  s_save = build_action_btn(group, "Save", kSaveW, kColorSaveOff, on_save,
                            &s_save_lbl);
  s_undo = build_action_btn(group, "Undo", kUndoW, kColorUndo, on_undo, nullptr);
  lv_obj_add_flag(s_undo, LV_OBJ_FLAG_HIDDEN);

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
  lv_obj_set_flex_align(back_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(back_row, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(back_row, LV_OBJ_FLAG_SCROLLABLE);
  build_back_chevron(back_row, kColorBack);
  lv_obj_t* back_lbl = lv_label_create(back_row);
  lv_obj_set_style_text_color(back_lbl, kColorBack, LV_PART_MAIN);
  lv_obj_set_style_text_font(back_lbl, &lv_font_montserrat_24, LV_PART_MAIN);
  lv_label_set_text(back_lbl, "Back");
  lv_obj_center(back_row);

  // Initial paint: nothing selected, Save disabled + centered, on-record line.
  refresh_type_pills();
  refresh_save_enabled();
  show_status();
  layout_actions();

  s_fade_n = 0;
  s_fade[s_fade_n++] = s_title;
  s_fade[s_fade_n++] = s_status;
  s_fade[s_fade_n++] = s_caption;
  s_fade[s_fade_n++] = s_pills[0].btn;
  s_fade[s_fade_n++] = s_pills[1].btn;
  s_fade[s_fade_n++] = s_pills[2].btn;
  s_fade[s_fade_n++] = s_date_hdr;
  s_fade[s_fade_n++] = s_minus;
  s_fade[s_fade_n++] = s_date;
  s_fade[s_fade_n++] = s_plus;
  s_fade[s_fade_n++] = s_save;
  s_fade[s_fade_n++] = s_undo;
  s_fade[s_fade_n++] = s_back;

  return group;
}

void refresh() {
  if (s_status == nullptr) return;
  s_days_ago = 0;       // re-enter the screen dialed to today
  s_has_sel  = false;   // and with no reason picked
  refresh_date_label();
  refresh_type_pills();
  refresh_save_enabled();
  show_status();        // re-read the store; clears any stale "saved" tint + Undo
  layout_actions();
}

lv_obj_t* const* fade_widgets(int* out_n) {
  *out_n = s_fade_n;
  return s_fade;
}

}  // namespace espressopost::ui::changes_screen
