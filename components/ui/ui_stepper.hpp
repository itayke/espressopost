#pragma once

#include "lvgl.h"

// Reusable value stepper — a [ (-) value (+) ] row whose value reads "--" until
// first touched, then "N<unit>". The value readout itself is tappable (a tap
// while "--" commits the seeded value; once set the discs own adjustment). The
// Post brew time and the preset editor's Weight In / Weight Out / Brew Time all
// instantiate one; the state lives in the caller so multiple instances coexist
// (the tap handlers pull their state off the widgets' user_data, not a global).
namespace espressopost::ui {

struct StepperState {
  uint8_t   value;       // current value, kept within [min, max]
  uint8_t   min;
  uint8_t   max;
  char      unit;        // suffix printed after the value ('s', 'g', …)
  bool      touched;     // false → "--" shown and the form gate held; sticky
  lv_obj_t* value_lbl;   // built by build_stepper; repainted by refresh
  void (*on_change)();   // fired after each tap (nullable) — re-eval the gate
};

// Visual config — Post uses a large value font; the editor a more compact one.
// The discs are outline circles `btn_diam` across, centered `btn_dx` either side
// of the row center.
struct StepperCfg {
  const lv_font_t* value_font;
  int32_t          btn_dx;
  int32_t          btn_diam;
};

// Build the row into a transparent container under `parent`; returns the
// container so the caller can position it. Wires the (-)/(+)/value handlers to
// `st`.
lv_obj_t* build_stepper(lv_obj_t* parent, StepperState* st,
                        const StepperCfg& cfg);

// Repaint the value readout from `st` ("--" muted while untouched, "N<unit>"
// else).
void stepper_refresh(StepperState* st);

}  // namespace espressopost::ui
