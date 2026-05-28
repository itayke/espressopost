#include "power.hpp"

#include "display.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

namespace espressopost::power {
namespace {

constexpr const char* kTag = "power";

// Policy knobs. See header comment for the full state machine.
constexpr uint8_t kBrightnessActive = 100;
constexpr uint8_t kBrightnessDimmed = 25;
constexpr int64_t kIdleToDimUs    =  30LL * 1000LL * 1000LL;
constexpr int64_t kIdleToOffUs    = 120LL * 1000LL * 1000LL;
constexpr int64_t kWakeDebounceUs = 500LL * 1000LL;
constexpr uint32_t kTickPeriodMs  = 500;

enum class State : uint8_t { kActive, kDimmed, kOff };

bool    s_inited = false;
State   s_state  = State::kActive;
int64_t s_last_activity_us       = 0;
int64_t s_wake_debounce_until_us = 0;
// True from a wake-tap (dim/off → active) until the touch driver reports
// the finger has lifted. The lift is observed in note_release(); the
// 500 ms safety floor (s_wake_debounce_until_us) covers the edge case
// where the controller never reports a clean release.
bool    s_awaiting_release       = false;

const char* state_name(State s) {
  switch (s) {
    case State::kActive: return "active";
    case State::kDimmed: return "dimmed";
    case State::kOff:    return "off";
  }
  return "?";
}

void apply_state(State s) {
  switch (s) {
    case State::kActive:
      // Turn the panel back on BEFORE bumping brightness so the first frame
      // shown is already at full intensity; the other order momentarily
      // shows the dimmed pixels from before the off transition.
      display::set_on(true);
      display::set_brightness(kBrightnessActive);
      break;
    case State::kDimmed:
      display::set_brightness(kBrightnessDimmed);
      break;
    case State::kOff:
      // Drop brightness first so any half-rendered frame between the two
      // calls doesn't strobe at full intensity.
      display::set_brightness(kBrightnessDimmed);
      display::set_on(false);
      break;
  }
}

void transition(State next) {
  if (next == s_state) return;
  ESP_LOGI(kTag, "%s -> %s", state_name(s_state), state_name(next));
  s_state = next;
  apply_state(next);
}

// LVGL timer — runs in the LVGL task, same task as the touch read callback,
// so we don't need to lock around s_state / s_last_activity_us.
void tick(lv_timer_t* /*t*/) {
  const int64_t now_us = esp_timer_get_time();
  const int64_t idle   = now_us - s_last_activity_us;
  switch (s_state) {
    case State::kActive:
      if (idle >= kIdleToDimUs) transition(State::kDimmed);
      break;
    case State::kDimmed:
      if (idle >= kIdleToOffUs) transition(State::kOff);
      break;
    case State::kOff:
      break;  // only consume_input() leaves this state
  }
}

}  // namespace

esp_err_t init() {
  if (s_inited) return ESP_ERR_INVALID_STATE;

  s_last_activity_us       = esp_timer_get_time();
  s_wake_debounce_until_us = 0;
  s_awaiting_release       = false;
  s_state                  = State::kActive;
  apply_state(s_state);

  if (!display::lock()) return ESP_ERR_TIMEOUT;
  const bool timer_ok = lv_timer_create(tick, kTickPeriodMs, nullptr) != nullptr;
  display::unlock();
  if (!timer_ok) return ESP_ERR_NO_MEM;

  s_inited = true;
  ESP_LOGI(kTag, "idle policy: dim @ %llds, off @ %llds",
           static_cast<long long>(kIdleToDimUs / 1'000'000),
           static_cast<long long>(kIdleToOffUs / 1'000'000));
  return ESP_OK;
}

bool consume_input() {
  if (!s_inited) return false;

  const int64_t now_us = esp_timer_get_time();
  s_last_activity_us   = now_us;

  // Both off→active and dimmed→active are "wake events": the user's tap
  // landed without intent to press whatever widget sits underneath. Treat
  // them identically — arm the release-edge gate and the time-safety floor.
  if (s_state == State::kOff || s_state == State::kDimmed) {
    transition(State::kActive);
    s_wake_debounce_until_us = now_us + kWakeDebounceUs;
    s_awaiting_release       = true;
    return true;
  }
  // Wake-tap finger hasn't been seen lifting yet — same physical touch.
  if (s_awaiting_release) {
    return true;
  }
  // Finger has lifted, but we're still inside the post-wake safety floor.
  if (now_us < s_wake_debounce_until_us) {
    return true;
  }
  return false;
}

void note_release() {
  if (!s_inited) return;
  s_awaiting_release = false;
}

}  // namespace espressopost::power
