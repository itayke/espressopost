# Screen scaling — Phase 0 inventory

Goal: make the UI recompilable for a circular screen of any size by routing
every screen-dimension literal through a single scale factor (currently 1.0,
targeting the 466 x 466 Waveshare AMOLED). This doc is the **map** — Phase 0,
identification only. No code changes yet.

## The anchor

Everything keys off [`ui_theme.hpp`](../components/ui/ui_theme.hpp) `kScreen = 466`.
The factor is `kScale = kScreen / 466.0` (== 1.0 today). The intended mechanism
for later phases is a rounding helper applied to each positional literal:

```cpp
constexpr int32_t S(int32_t px) { return (int32_t)(px * kScale + 0.5f); }
```

## What already scales / doesn't care (no work needed)

- The grind dial is a **horizontal bar**, not an arc/ring — there is no polar or
  radius geometry to re-derive.
- [`ui_stepper.cpp`](../components/ui/ui_stepper.cpp) is fully **parameterized**
  (`diam`, `btn_dx` passed in by the caller) — it inherits scaling for free from
  whatever constants its callers feed it.
- Some constants are already **derived**: `kCenter`, `kCenterLineY`,
  `kCenterLineOffsetY`, `kPrimaryBtnRightInset`, plus inline forms like
  `kScreen - 2*kSeparatorInset` and `kQrSize + 2*kQrPad`.

---

## Class 1 — Positions & sizes (scale by factor)

Coordinates, widths, heights, diameters, insets, gaps. ~120 constants; each a
clean single multiply through `S()`.

### `ui_theme.hpp` (shared frame — highest leverage)
- `kGrinderSeparatorY=305`, `kClimateSeparatorY=215` (L23-24)
- `kPostBtnW=120`, `kPostBtnH=58`, `kCenterEdgeInset=30` (L42-46)

### `ui_report.cpp` (49 consts)
- Idle: `kGrindValueY=108`, `kGrindCaptionX=50`, `kGrindCaptionY=335`, `kBarY=384`
- Suggestion pill: `kSuggestionBtnW=130`, `kSuggestionBtnH=52`, `kSuggestionBtnX=300`, `kSuggestionBtnY=312`
- Stars: `kStarSize=38`, `kStarGap=8`
- Cursor/suggestion arrows: `kCursorWidget=20`, `kCursorArrowHalfBase=10`, `kCursorArrowHeight=24`, `kCursorTipGap=17`, `kSuggestionArrowHalfBase=6`, `kSuggestionArrowHeight=14`
- Popup: `kPopupCardW=320`, `kPopupPad=22`, `kPopupGap=16`, `kPopupBtnGap=14`
- Climate strip: `kClimateAreaHeight=210`, `kColLeftEdge0/1/2 = 0/155/311` (approx thirds of 466), `kTileIconY=46`, `kTileLabelY=108`, `kTileValueY=130`, `kIconSize=60`, `kSeparatorInset=10`, `kOuterContentShift=-5`
- Post form: `kBrewCaptionTopY=25`, `kBrewRowCenterY=74`, `kBrewBtnDX=90`, `kQualityCaptionY=125`, `kStarRowY=155`, `kPillRowGap=6`, `kPillTextPaddingX=27`, `kPillRowMidY=175`, `kStarsToPillsGap=30`
- Animation slides: `kColTopOff100=-18`, `kIntroTextSlideY=15`, `kPresetFlipSlideY=20`

### `ui_preset_edit.cpp`
- `kTitleTopY=22`, `kStepperBtnDX=86`, `kInCenterY=118`, `kOutCenterY=222`, `kBrewCenterY=326`, `kCaptionDY=46`, `kWeightArrowW=12`, `kWeightArrowH=16`, `kSwatchDiam=40`, `kSwatchSelDiam=50`, `kSwatchPitch=60`, `kSwatchColX=70`, `kBtnBottomInset=26`, `kSaveBtnW=110`, `kBtnGap=12`

### `ui_changes.cpp`
- `*TopY` (`30,66,86,126,152,220,229,247,310`), `kStepBtnSize=56`, `kStepperDX=128`, `kActionBtnH=66`, `kSaveW=150`, `kUndoW=150`, `kActionGap=16`, `kBackBtnW=132`, `kBackBtnH=56`, `kBackBtnBottomInset=16`

### `ui_presets.cpp`
- `kHamburgerW=26`, `kHamburgerH=18`, `kHamburgerGap=6`, `kPresetsTitleTopY=30`, `kGridCell=92`, `kGridGap=12`, `kGridTopY=78`, `kSlotRadius=14`, `kSlotPad=4`, `kBackBtn*`

### `ui_connections.cpp`
- `kTitleTopY=30`, `kWifiLineTopY=92`, `kRssiLineTopY=124`, `kSyncLineTopY=156`, `kHintLineTopY=352`, `kConnectBtnW=244`, `kConnectBtnH=62`, `kForgetCenterDY=74`, `kQrSize=150`, `kQrPad=12`, `kQrCenterDY=4`, `kBackBtn*`, inline `radius=8` (L167)

### `ui_menu.cpp`
- `kTitleTopY=30`, `kClockTopY=66`, `kShotsTopY=90`, `kEntryW=264`, `kEntryH=66`, `kEntryGap=14`, `kEntryFirstTopY=124`, `kBackBtn*`

### `ui_preset_readout.cpp`
- `kArrowW=12`, `kArrowH=10`, inline `pad_column=4`, `pad_row=2`

### `ui_bar.hpp`
- `kBarHalfWidth=166`, `kBarStripHeight=36`, `kBarStripBgHeight=14`, tick lengths `kBigTickLen=24` / `kMidTickLen=18` / `kSmallTickLen=8` / `kTinyTickLen=2`

### Magic margin
- The literal `80` in `kScreen - 80` appears inline 3x
  ([`ui_changes.cpp:444,454`](../components/ui/ui_changes.cpp#L444),
  [`ui_connections.cpp:87`](../components/ui/ui_connections.cpp#L87)) — should
  become a named `kSideMargin` and scale.

---

## Class 2 — Strokes / hairlines (scale, but floor at 1px)

Thin lines vanish if scaled down and look heavy if scaled up, so these want
`max(1, S(x))` rather than raw scaling:

- Theme: `kPostBtnStroke=4`, `kPostBtnExtClick=10`
- `kSuggestionBtnStroke=2`, `kSeparatorThickness=2`, `kPillButtonStroke=2`
- Hit-pads: `kStarExtClick` / `kPillExtClick` / `kTypeExtClick` = 10
- Bar tick thicknesses: `kBigTickThickness=3`, `kMidTickThickness=1`, `kSmallTickThickness=1`
- `kSlotBorder=2`, `kTypePillStroke=2`, `kSwatchSelStroke=2`
- The duplicated local `kStroke=3` / back-chevron glyph `kW=11, kH=18, kStroke=3`
  (copy-pasted in 4 files: presets, menu, connections, changes)

---

## Class 3 — Fonts (discrete — the hard part)

`lv_font_montserrat_` 14 (x20 uses), 24 (x30), 36 (x1), 46 (x3). LVGL fonts are
**compiled bitmaps**, so this axis can't continuously scale from a single
multiply. Options for the next phase:

1. `pick_font(S(size))` that snaps to the nearest enabled montserrat size, or
2. Enable additional montserrat sizes in `sdkconfig` and select per scale band.

This is the one axis that won't fall out of `S()`.

---

## Class 4 — DO NOT scale (value-domain & counts)

These match an `int32_t = number` pattern but are **data, not pixels**. A blanket
`S()` would corrupt behavior:

- `kDoseMin/Max/Def`, `kYieldMin/Max/Def`, `kBrewTimeMin/Max/Def`
  ([`ui_preset_edit.cpp:23-33`](../components/ui/ui_preset_edit.cpp#L23))
- The grind range in `kGrindSpec`
- Layout **counts**: `kNumSwatches=10`, `kSwatchPerCol=5`, `kGridCols=3`

---

## Tally

~140 positional constants across 9 UI files:

| Class | Count (approx) | Treatment |
| --- | --- | --- |
| 1 — positions & sizes | ~120 | single multiply via `S()` |
| 2 — strokes / hairlines | ~15 | `max(1, S(x))` |
| 3 — fonts | 4 sizes | nearest-enabled lookup (non-linear) |
| 4 — value-domain / counts | ~12 | leave literal |

## Suggested prep before Phase 1 (makes it mechanical)

1. Pull the duplicated `kBackBtn*` and the back-chevron glyph into `ui_theme.hpp`.
2. Name the `80` side-margin (`kSideMargin`).
3. Decide the font strategy (Class 3) up front — it gates how far "any size"
   actually goes.
