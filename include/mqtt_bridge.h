#pragma once

#include <stdint.h>
#include "hub_controls.h"

namespace s11mqtt {

constexpr char FIRMWARE_VERSION[] = "5.6";

// MQTT ima svoj task in vrsto; RF task tu nikoli ne caka na omrezje.
void begin(const hubcontrol::LedSettings &settings);
bool take_control(hubcontrol::Command &command);
void publish_led_settings(const hubcontrol::LedSettings &settings);
void enqueue_press(uint8_t switch_id, uint32_t count, uint32_t captured_at_ms);
void set_paused(bool paused);
void service_reports();

}  // namespace s11mqtt
