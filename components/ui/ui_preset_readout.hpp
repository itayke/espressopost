#pragma once

#include <cstdint>

#include "lvgl.h"
#include "presets.hpp"

// Preset readout — the "PRESET N / dose → yield / time" block shown in three
// places: the idle center-line preset button, the post-mode read-only surface,
// and each Presets-screen grid slot. PresetReadout is the handle bundle; the two
// builders lay the same fields out differently (wide center line vs compact grid
// cell); apply_readout pushes a preset's values + accent color into either.
namespace espressopost::ui {

struct PresetReadout {
  lv_obj_t* root;   // outer flex column container
  lv_obj_t* top;    // "PRESET N" label
  lv_obj_t* dose;   // "Xg" label
  lv_obj_t* yield;  // "Xg" label
  lv_obj_t* time;   // " Ys" label (leading space — visually separates the brew ratio from the time)
  lv_obj_t* arrow;  // dose→yield arrow container (two lv_line strokes), recolored per preset
};

// Center-line variant: "PRESET N" (MS24) over a single
// [dose | → | yield | time] row (MS14). Used by the idle preset button and the
// post-mode read-only surface.
PresetReadout build_preset_readout(lv_obj_t* parent);

// Grid variant: compact, everything at MS14 with the time pushed to its own
// third line — fits a small square Presets-screen slot.
PresetReadout build_preset_readout_grid(lv_obj_t* parent);

// Push a preset's values AND its accent color into a readout. `slot` is the
// 0-based id (rendered 1-based as "PRESET N").
void apply_readout(PresetReadout& r, const presets::Preset& p, uint8_t slot);

// Empty-slot variant: show only the "PRESET N" label tinted `color` (the slot's
// frame color) and hide the value rows. Used by the Presets grid so an unused
// slot still reads "PRESET N" in the muted frame hue.
void apply_readout_empty(PresetReadout& r, uint8_t slot, lv_color_t color);

}  // namespace espressopost::ui
