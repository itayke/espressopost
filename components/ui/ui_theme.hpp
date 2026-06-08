#pragma once

#include "lvgl.h"

// Shared theme — only the layout frame and base palette that EVERY mode sits
// inside. Mode-specific geometry and accent colors live with their mode in
// ui_report.cpp (one tuning section per mode), so they travel together if a
// mode is later split into its own file.
namespace espressopost::ui {

// ---------------------------------------------------------------------------
// Layout frame — round AMOLED, kScreen × kScreen.
// ---------------------------------------------------------------------------
constexpr int32_t kScreen = 466;
constexpr int32_t kCenter = kScreen / 2;

// Horizontal dividers framing the center line — climate area sits above
// kClimateSeparatorY, grinder area below kGrinderSeparatorY, with the center
// button row parked midway between them (kCenterLineY). kGrinderSeparatorY is
// purely visual — per-bar PRESSED hit-test uses BarSpec.y_band_{top,bottom}.
// Tune these two Y's and the center line + climate area position track them
// automatically.
constexpr int32_t kGrinderSeparatorY = 305;
constexpr int32_t kClimateSeparatorY = 215;

// Vertical midline between the two separators — where the center button row
// (POST + preset btn in idle, ✕/preset/Submit in post) sits. NOT the screen's
// geometric center: the grinder area is taller than the climate strip, so a
// true mid-screen line would land low inside the grinder band.
// kCenterLineOffsetY is the offset to feed LV_ALIGN_CENTER when placing widgets
// onto this line.
constexpr int32_t kCenterLineY       =
    (kClimateSeparatorY + kGrinderSeparatorY) / 2;
constexpr int32_t kCenterLineOffsetY = kCenterLineY - kCenter;

// Shared center-line button geometry. POST (idle), Submit (post), and the
// ✕ Cancel pill (post) all share kPostBtnW × kPostBtnH so the row keeps the
// same footprint when the top area swaps modes. kPostBtnStroke is the
// outline-only border width; kPostBtnExtClick the hit-area pad on every side of
// the action buttons (POST, ✕ Cancel, Submit, (-)/(+) steppers) — the visual
// pill stays kPostBtnW × kPostBtnH while the click target grows by this amount.
constexpr int32_t kPostBtnW          = 120;
constexpr int32_t kPostBtnH          =  58;
constexpr int32_t kPostBtnStroke     =   4;
constexpr int32_t kPostBtnExtClick   =  10;
constexpr int32_t kCenterEdgeInset   =  30;
// Right-edge inset for the primary action pills (idle POST / post Submit).
// Tighter than kCenterEdgeInset so the armed action sits a touch closer to the
// screen edge than the general center-line inset.
constexpr int32_t kPrimaryBtnRightInset = kCenterEdgeInset - 15;

// ---------------------------------------------------------------------------
// Base palette — AMOLED-friendly: pure-black background, no max-intensity
// sub-pixels (saves burn-in). COLOR(0xRRGGBB) is a thin shim over LV_COLOR_MAKE
// so palette entries read like a CSS hex literal instead of three comma-
// separated bytes — easier to paste a Figma swatch in unchanged. Only the
// cross-mode base tiers live here; per-element accents stay with their mode.
// ---------------------------------------------------------------------------
#define COLOR(rgb) LV_COLOR_MAKE(((rgb) >> 16) & 0xFF, \
                                 ((rgb) >> 8)  & 0xFF, \
                                 (rgb)         & 0xFF)

inline const lv_color_t kColorBg      = COLOR(0x000000);
inline const lv_color_t kColorText    = COLOR(0xE0E0E0);
inline const lv_color_t kColorSubText = COLOR(0xA0A0A0);
inline const lv_color_t kColorLabel   = COLOR(0xB0B0B0);
inline const lv_color_t kColorMuted   = COLOR(0x707070);
inline const lv_color_t kColorMuted2  = COLOR(0x505050);
inline const lv_color_t kColorMuted3  = COLOR(0x303030);
inline const lv_color_t kColorMuted4  = COLOR(0x202020);

}  // namespace espressopost::ui
