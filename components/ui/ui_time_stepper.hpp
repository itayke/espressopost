#pragma once

#include "lvgl.h"

// Reusable brew-time stepper — a [ (-) value (+) ] row whose value reads "--"
// until first touched, then "Ns". The value readout itself is tappable (a tap
// while "--" commits the seeded value; once set the discs own adjustment). The
// Post form and the preset editor both instantiate one; the state lives in the
// caller so multiple instances coexist (the tap handlers pull their state off
// the widgets' user_data, not a global).
namespace espressopost::ui {

struct TimeStepperState {
  uint8_t   value_s;     // current value in seconds, kept within [min_s, max_s]
  uint8_t   min_s;
  uint8_t   max_s;
  bool      touched;     // false → "--" shown and the form gate held; sticky
  lv_obj_t* value_lbl;   // built by build_time_stepper; repainted by refresh
  void (*on_change)();   // fired after each tap (nullable) — re-eval the gate
};

// Visual config — Post uses a large value font; the editor a more compact one.
// The discs are outline circles `btn_diam` across, centered `btn_dx` either side
// of the row center.
struct TimeStepperCfg {
  const lv_font_t* value_font;
  int32_t          btn_dx;
  int32_t          btn_diam;
};

// Build the row into a transparent container under `parent`; returns the
// container so the caller can position it. Wires the (-)/(+)/value handlers to
// `st`.
lv_obj_t* build_time_stepper(lv_obj_t* parent, TimeStepperState* st,
                             const TimeStepperCfg& cfg);

// Repaint the value readout from `st` ("--" muted while untouched, "Ns" else).
void time_stepper_refresh(TimeStepperState* st);

}  // namespace espressopost::ui
