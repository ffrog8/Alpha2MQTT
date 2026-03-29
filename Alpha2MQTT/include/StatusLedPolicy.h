// Purpose: Define the status LED render policy for transport/runtime state and
// short-lived activity overlays.
// Responsibilities: Keep the base-color mapping and MQTT activity pulse
// behavior host-testable instead of embedding it in platform-specific GPIO code.
// Invariants: RGB builds can show a blue MQTT pulse directly; monochrome builds
// expose the same event as a brief visible blink via the boolean output.
#pragma once

#include <cstdint>

struct StatusLedRender {
	uint8_t red = 0;
	uint8_t green = 0;
	uint8_t blue = 0;
	bool monochromeOn = false;
};

StatusLedRender computeStatusLedRender(bool wifiConnected,
                                       bool mqttConnected,
                                       bool rs485Connected,
                                       bool mqttActivityPulseActive);
