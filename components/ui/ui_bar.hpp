#pragma once

#include "lvgl.h"
#include "ui_theme.hpp"  // kCenter, kColorBg / COLOR (tick colors derive from it)

// Generic scroll-with-momentum bar engine — drag, flick, snap, and custom tick
// paint. BarSpec is the value-domain config (range, snap step, tick tiers,
// screen y + hit-band); BarState is per-instance runtime. The grind dial is the
// only consumer today, but the chassis stays generic so a second bar can drop
// in by supplying its own BarSpec + hooks. All bar visual + feel tuning lives in
// the one section below.
namespace espressopost::ui {

// ===== BAR TUNING ==========================================================
// Bar widget — horizontal strip, total width 2·kBarHalfWidth, kBarStripHeight
// tall. Small-tick spacing is kBarHalfWidth/10 px at the grind density.
constexpr int32_t kBarHalfWidth     = 166;
constexpr int32_t kBarStripHeight   = 36;
// Background rail height — matches the small tick so the rail reads as the
// substrate those ticks sit on; big and mid ticks extend past it.
constexpr int32_t kBarStripBgHeight = 14;

// Tick sizes. Big = integer landmark (taller AND thicker), mid = half-step
// "long hairline", small = fine hairline, tiny = sub-step texture (borrows the
// small color/thickness but shorter).
constexpr int32_t kBigTickLen         = 24;
constexpr int32_t kBigTickThickness   = 3;
constexpr int32_t kMidTickLen         = 18;
constexpr int32_t kMidTickThickness   = 1;
constexpr int32_t kSmallTickLen       = 8;
constexpr int32_t kSmallTickThickness = 1;
constexpr int32_t kTinyTickLen        = 2;

// Tick colors — all the bg color (black ticks bite out of the amber rail); mid
// + small share a tier so half-unit ticks read as longer hairlines.
inline const lv_color_t kBigTickColor    = kColorBg;
inline const lv_color_t kMidTickColor    = kColorBg;
inline const lv_color_t kSmallTickColor  = kColorBg;
inline const lv_color_t kTinyTickColor   = kColorBg;
inline const lv_color_t kBarStripBgColor = COLOR(0xAE6923);

// Flick momentum cadence + decay — shared by every bar; per-bar runtime lives
// in BarState.
constexpr uint32_t kMomentumPeriodMs = 30;     // tick cadence
constexpr int      kMomentumMaxTicks = 17;     // ≈500 ms at 30 ms/tick
constexpr float    kMomentumDecay    = 0.85f;  // per tick → ~6% left after 17 ticks
constexpr float    kMomentumMinSpeed = 0.5f;   // value units/sec — below this we stop
// ===========================================================================

struct BarSpec {
  float    min;
  float    max;
  float    step;                  // snap grid; value rounds here at settle
  float    visible_half_range;    // value units shown from cursor center to bar edge
  int32_t  y;                     // screen y of bar centerline
  // PRESSED hit-test band in screen y. Wider than the visible bar so a slightly
  // mistargeted swipe still lands. A second bar's band must NOT overlap this
  // one, or a single press would race both bars' drag flags.
  int32_t  y_band_top;
  int32_t  y_band_bottom;
  int      big_every;             // 1-of-N tick indices is "big"
  int      mid_every;             // 1-of-N tick indices is "mid"
  int      small_every;           // 1-of-N is "small"; the rest are "tiny".
                                  // Sentinel 0 disables the tiny tier — every
                                  // non-big/non-mid index renders as small.
  float    tick_unit;             // value step between adjacent ticks
};

struct BarState;
using BarHook = void (*)(BarState*);

struct BarState {
  const BarSpec* spec;
  lv_obj_t* widget;        // custom-drawn lv_obj; tick paint goes here
  float     value;         // free-floating during drag/glide; snapped to spec.step on settle
  bool      touched;       // sticky — was this bar swiped since the last form reset?

  // Drag bookkeeping. `dragging` is set in PRESSED only when the press lands
  // inside spec.y_band_{top,bottom}, cleared in RELEASED / PRESS_LOST.
  // PRESSING is a no-op until `dragging` is true.
  bool      dragging;
  int32_t   last_x;
  uint64_t  last_us;

  // Flick momentum — kMomentum* cadence/decay.
  float       velocity;            // value units / sec, signed
  lv_timer_t* momentum_timer;
  int         momentum_ticks_left;

  // Hooks. on_change fires whenever value moves (drag step, momentum step, or
  // settle-snap). on_settle fires once when drag/glide ends, after snap.
  // on_touched fires once per form session when `touched` flips false→true.
  // All nullable.
  BarHook on_change;
  BarHook on_settle;
  BarHook on_touched;
};

// Build the tick-strip lv_obj for `state` under `parent`. The caller wires
// state->spec / value / hooks before the first paint.
lv_obj_t* make_bar_widget(lv_obj_t* parent, BarState* state);

// Feed a touch event (from the consumer's overlay / widget callback) to bar `s`:
// drag, velocity tracking, flick handoff, snap. Hooks fire as the value moves.
void bar_dispatch_event(lv_event_t* e, BarState* s);

}  // namespace espressopost::ui
