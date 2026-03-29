// Purpose: Provide a single render decision for the shared status LED.
// Chose a pure helper over inline logic in main.cpp so the MQTT activity pulse
// remains testable on host builds without Arduino GPIO dependencies.

#include "../include/StatusLedPolicy.h"

StatusLedRender
computeStatusLedRender(bool wifiConnected,
                       bool mqttConnected,
                       bool rs485Connected,
                       bool mqttActivityPulseActive)
{
	if (mqttActivityPulseActive) {
		return { 0, 0, 255, true };
	}

	if (!wifiConnected) {
		return { 255, 0, 0, false };
	}
	if (!mqttConnected) {
		return { 255, 255, 0, false };
	}
	if (!rs485Connected) {
		return { 128, 0, 128, false };
	}
	return { 0, 255, 0, false };
}
