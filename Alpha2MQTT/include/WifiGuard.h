// Wifi guard policy helpers.
// Purpose: keep WiFi scanning decisions centralized and testable.
// Invariant: NORMAL mode must not start scans to avoid heap pressure, but
// any in-flight async scan must still be guarded to avoid stale callbacks.
#pragma once

#include <cstdint>

#include "BootModes.h"

constexpr bool shouldStartWifiScan(BootMode mode)
{
	return mode != BootMode::Normal;
}

enum class WifiScanGuardAction : uint8_t {
	None = 0,
	RebindNoopCallback = 1,
	DeleteResults = 2,
};

constexpr int8_t kWifiScanRunningState = -1;
constexpr int8_t kWifiScanFailedState = -2;

constexpr WifiScanGuardAction classifyWifiScanGuard(int8_t scanState)
{
	if (scanState == kWifiScanRunningState) {
		return WifiScanGuardAction::RebindNoopCallback;
	}
	if (scanState >= 0 || scanState == kWifiScanFailedState) {
		return WifiScanGuardAction::DeleteResults;
	}
	return WifiScanGuardAction::None;
}
