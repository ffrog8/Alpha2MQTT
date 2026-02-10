// Wifi guard policy helpers.
// Purpose: keep WiFi scanning decisions centralized and testable.
// Invariant: NORMAL mode must not start scans to avoid heap pressure.
#pragma once

#include "BootModes.h"

constexpr bool shouldStartWifiScan(BootMode mode)
{
	return mode != BootMode::Normal;
}
