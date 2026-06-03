#pragma once

#include "lvgl.h"

// Connections screen (the Mode::Connections view): shows WiFi + cloud-sync
// status and a "Connect Wi-Fi" action that kicks off SoftAP provisioning.
// Reached from the Menu hub. Like the other panel views, this module owns the
// look + fade set; ui_report drives mode/navigation and injects the handlers,
// and polls refresh() while the screen is visible so live status updates show.
namespace espressopost::ui::connections_screen {

// Build under `scr`. Back's CLICKED → `on_back`; the Connect pill → `on_connect`
// (which starts provisioning). Returns the group handle for the mode swap.
lv_obj_t* build(lv_obj_t* scr, lv_event_cb_t on_back, lv_event_cb_t on_connect);

// Re-read cloud::status() and repaint the status lines. Cheap; call at the swap
// midpoint (before fade-in) and on a timer while the screen is visible.
void refresh();

// The screen's fade set (title + status lines + Connect + Back).
lv_obj_t* const* fade_widgets(int* out_n);

}  // namespace espressopost::ui::connections_screen
