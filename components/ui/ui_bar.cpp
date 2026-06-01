#include "ui_bar.hpp"

#include <algorithm>
#include <cmath>

#include "esp_timer.h"

namespace espressopost::ui {
namespace {

// Generic bar tick painter. Pulls BarState* off the widget's user_data, reads
// the spec's range / tick rules, and emits the bg rail + tick rects centered on
// the widget's coord box. Out-of-range ticks (below spec.min or above spec.max)
// are clipped so a bar whose range starts at 0 doesn't bleed ticks below the
// floor when the cursor sits there.
void draw_bar_event(lv_event_t* e) {
  lv_layer_t* layer = lv_event_get_layer(e);
  if (layer == nullptr) return;
  auto* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
  // BarState lives on the WIDGET (lv_obj_set_user_data in make_bar_widget), not
  // on the callback registration. lv_event_get_user_data would return the
  // per-callback pointer (nullptr in our registration).
  auto* state = static_cast<BarState*>(lv_obj_get_user_data(obj));
  if (state == nullptr || state->spec == nullptr) return;
  lv_area_t coords;
  lv_obj_get_coords(obj, &coords);
  const int32_t cx = coords.x1 + lv_area_get_width(&coords)  / 2;
  const int32_t cy = coords.y1 + lv_area_get_height(&coords) / 2;

  const BarSpec& spec = *state->spec;
  const float    value = state->value;
  const float    px_per_unit =
      static_cast<float>(spec.half_width) / spec.visible_half_range;
  const float    inv_tick = 1.0f / spec.tick_unit;

  // Background rail. Drawn first so the ticks paint on top; sized to the
  // small-tick height so the big and mid ticks still stick proud of the rail.
  // Pill ends — LV_RADIUS_CIRCLE rounds both caps to half-height without
  // touching the straight middle section.
  {
    lv_draw_rect_dsc_t bg_dsc;
    lv_draw_rect_dsc_init(&bg_dsc);
    bg_dsc.bg_color = kBarStripBgColor;
    bg_dsc.bg_opa   = LV_OPA_COVER;
    bg_dsc.radius   = LV_RADIUS_CIRCLE;
    lv_area_t bg_a = {cx - spec.half_width, cy - kBarStripBgHeight / 2,
                      cx + spec.half_width, cy + kBarStripBgHeight / 2};
    lv_draw_rect(layer, &bg_dsc, &bg_a);
  }

  // Visible range of values across the bar, plus one tick of over-scan so a
  // tick doesn't pop in/out at the very edge as the value scrolls.
  const int32_t idx_min = static_cast<int32_t>(
      std::floor((value - spec.visible_half_range) * inv_tick)) - 1;
  const int32_t idx_max = static_cast<int32_t>(
      std::ceil((value + spec.visible_half_range) * inv_tick)) + 1;

  lv_draw_rect_dsc_t big_dsc;
  lv_draw_rect_dsc_init(&big_dsc);
  big_dsc.bg_color = kBigTickColor;
  big_dsc.bg_opa   = LV_OPA_COVER;

  lv_draw_rect_dsc_t mid_dsc;
  lv_draw_rect_dsc_init(&mid_dsc);
  mid_dsc.bg_color = kMidTickColor;
  mid_dsc.bg_opa   = LV_OPA_COVER;

  lv_draw_rect_dsc_t small_dsc;
  lv_draw_rect_dsc_init(&small_dsc);
  small_dsc.bg_color = kSmallTickColor;
  small_dsc.bg_opa   = LV_OPA_COVER;

  lv_draw_rect_dsc_t tiny_dsc;
  lv_draw_rect_dsc_init(&tiny_dsc);
  tiny_dsc.bg_color = kTinyTickColor;
  tiny_dsc.bg_opa   = LV_OPA_COVER;

  // Half a tick of slack on the value-domain clip so a tick that sits exactly
  // at spec.min / spec.max still draws (floating-point rounding shouldn't erase
  // the floor tick).
  const float clip_slack = spec.tick_unit * 0.5f;

  for (int32_t idx = idx_min; idx <= idx_max; ++idx) {
    const float v_i = idx * spec.tick_unit;
    if (v_i < spec.min - clip_slack) continue;
    if (v_i > spec.max + clip_slack) continue;
    // Standard slider direction: HIGHER value sits RIGHT of cursor.
    const float x_offset = (v_i - value) * px_per_unit;
    if (std::fabs(x_offset) > static_cast<float>(spec.half_width) + 0.5f) continue;
    const int32_t tick_x = cx + static_cast<int32_t>(std::lround(x_offset));
    // Tier: big_every > mid_every > small_every (when set; the rest are tiny).
    // When small_every == 0 the tiny tier is disabled and every non-big/non-mid
    // index falls through as small. Tiny shares the small color + thickness;
    // only length differs.
    const bool is_big   = (idx % spec.big_every == 0);
    const bool is_mid   = !is_big && (idx % spec.mid_every == 0);
    const bool is_small = !is_big && !is_mid &&
        (spec.small_every == 0 || idx % spec.small_every == 0);
    int32_t half_h, half_w;
    lv_draw_rect_dsc_t* dsc;
    if (is_big) {
      half_h = kBigTickLen / 2;
      half_w = kBigTickThickness / 2;
      dsc    = &big_dsc;
    } else if (is_mid) {
      half_h = kMidTickLen / 2;
      half_w = kMidTickThickness / 2;
      dsc    = &mid_dsc;
    } else if (is_small) {
      half_h = kSmallTickLen / 2;
      half_w = kSmallTickThickness / 2;
      dsc    = &small_dsc;
    } else {
      half_h = kTinyTickLen / 2;
      half_w = kSmallTickThickness / 2;
      dsc    = &tiny_dsc;
    }
    lv_area_t a = {tick_x - half_w, cy - half_h,
                   tick_x + half_w, cy + half_h};
    lv_draw_rect(layer, dsc, &a);
  }
}

void bar_cancel_momentum(BarState* s) {
  if (s->momentum_timer != nullptr) {
    lv_timer_delete(s->momentum_timer);
    s->momentum_timer = nullptr;
  }
  s->velocity = 0.0f;
  s->momentum_ticks_left = 0;
}

void bar_snap_and_settle(BarState* s) {
  const float snapped = std::round(s->value / s->spec->step) * s->spec->step;
  if (snapped != s->value) {
    s->value = snapped;
    if (s->on_change) s->on_change(s);
  }
  s->velocity = 0.0f;
  if (s->on_settle) s->on_settle(s);
}

void bar_momentum_tick(lv_timer_t* t) {
  auto* s = static_cast<BarState*>(lv_timer_get_user_data(t));
  if (s == nullptr || s->spec == nullptr) return;
  const BarSpec& spec = *s->spec;
  const float dt_s = static_cast<float>(kMomentumPeriodMs) / 1000.0f;
  const float new_value =
      std::clamp(s->value + s->velocity * dt_s, spec.min, spec.max);
  if (new_value != s->value) {
    s->value = new_value;
    if (s->on_change) s->on_change(s);
  }
  s->velocity *= kMomentumDecay;
  --s->momentum_ticks_left;

  // Stop on any of: out of ticks, decayed below noise floor, or pinned to a
  // range edge. Snap + persist before the timer self-destructs.
  const bool at_edge = (s->value <= spec.min + 1e-4f) ||
                       (s->value >= spec.max - 1e-4f);
  if (s->momentum_ticks_left <= 0 ||
      std::fabs(s->velocity) < kMomentumMinSpeed ||
      at_edge) {
    bar_snap_and_settle(s);
    s->momentum_timer = nullptr;
    lv_timer_delete(t);
  }
}

}  // namespace

// Make one bar's tick-strip widget. Parent decides visibility — the grind bar
// goes on the screen (visible in both modes). The BarState's spec/widget/hooks
// must be wired by the caller before lv_obj_invalidate triggers a paint.
lv_obj_t* make_bar_widget(lv_obj_t* parent, BarState* state) {
  lv_obj_t* w = lv_obj_create(parent);
  lv_obj_set_size(w, 2 * state->spec->half_width, kBarStripHeight);
  lv_obj_set_pos(w, state->spec->center_x - state->spec->half_width,
                 state->spec->y - kBarStripHeight / 2);
  lv_obj_set_style_bg_opa(w, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(w, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(w, 0, LV_PART_MAIN);
  lv_obj_clear_flag(w, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(w, LV_OBJ_FLAG_CLICKABLE);
  // user_data carries the BarState* so draw_bar_event can pull the value / spec
  // without globals.
  lv_obj_set_user_data(w, state);
  lv_obj_add_event_cb(w, draw_bar_event, LV_EVENT_DRAW_MAIN, nullptr);
  return w;
}

void bar_dispatch_event(lv_event_t* e, BarState* s) {
  if (s == nullptr || s->spec == nullptr) return;
  const BarSpec& spec = *s->spec;
  const auto code = lv_event_get_code(e);
  lv_indev_t* indev = lv_indev_active();
  if (indev == nullptr) return;
  lv_point_t p;
  lv_indev_get_point(indev, &p);

  switch (code) {
    case LV_EVENT_PRESSED: {
      // Only react when the press lands in THIS bar's y-band. Outside the band
      // the bar stays asleep so the other bar (or no bar at all) can claim the
      // press.
      if (p.y < spec.y_band_top || p.y > spec.y_band_bottom) {
        s->dragging = false;
        return;
      }
      // Grabbing again mid-glide stops the flywheel — the new touch should own
      // the motion, not fight a tail from the last release.
      bar_cancel_momentum(s);
      s->dragging = true;
      s->last_x   = p.x;
      s->last_us  = esp_timer_get_time();
      // First touch in this form session — fire the hook so the Post form can
      // enable Submit. Subsequent touches don't re-fire (touched is sticky).
      if (!s->touched) {
        s->touched = true;
        if (s->on_touched) s->on_touched(s);
      }
      break;
    }
    case LV_EVENT_PRESSING: {
      if (!s->dragging) return;
      const int32_t dx_px = p.x - s->last_x;
      s->last_x = p.x;
      // Direct manipulation: the bar follows the finger. Finger right (positive
      // dx) scrolls ticks RIGHT under the fixed center cursor, which reads a
      // LOWER value — hence the sign flip on dv.
      //
      // Sub-step motion is fine; readout labels round for display in their own
      // refresh. Snap happens at release / momentum-end.
      const float px_per_unit =
          static_cast<float>(spec.half_width) / spec.visible_half_range;
      const float dv = -static_cast<float>(dx_px) / px_per_unit;
      const float new_value =
          std::clamp(s->value + dv, spec.min, spec.max);
      if (new_value != s->value) {
        s->value = new_value;
        if (s->on_change) s->on_change(s);
      }
      // Track velocity for the post-release flick. EMA with α=0.5 smooths
      // single-frame jitter while still responding within ~2-3 frames.
      const uint64_t now_us = esp_timer_get_time();
      if (s->last_us != 0) {
        const float dt_s = static_cast<float>(now_us - s->last_us) / 1e6f;
        if (dt_s > 1e-4f) {
          const float instant = dv / dt_s;
          s->velocity = 0.5f * s->velocity + 0.5f * instant;
        }
      }
      s->last_us = now_us;
      break;
    }
    case LV_EVENT_RELEASED:
    case LV_EVENT_PRESS_LOST: {
      if (!s->dragging) break;
      s->dragging = false;
      s->last_us  = 0;
      // If the finger lifted with non-trivial speed, hand off to the momentum
      // timer; it'll snap + settle when it dies. Otherwise settle now.
      if (std::fabs(s->velocity) >= kMomentumMinSpeed) {
        s->momentum_ticks_left = kMomentumMaxTicks;
        if (s->momentum_timer == nullptr) {
          s->momentum_timer =
              lv_timer_create(bar_momentum_tick, kMomentumPeriodMs, s);
        }
      } else {
        bar_snap_and_settle(s);
      }
      break;
    }
    default:
      break;
  }
}

}  // namespace espressopost::ui
