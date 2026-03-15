#include "../include/BootModes.h"

#include <cstring>

SubsystemStates decideSubsystems(BootMode mode)
{
	switch (mode) {
	case BootMode::Normal:
		return { true, true, false, false };
	case BootMode::ApConfig:
		return { false, false, true, false };
	case BootMode::WifiConfig:
		return { false, false, true, true };
	default:
		return { false, false, false, false };
	}
}

SubsystemPlan
planForBootMode(BootMode mode)
{
	switch (mode) {
	case BootMode::Normal:
		return { true, true, true };
	case BootMode::ApConfig:
		return { true, false, false };
	case BootMode::WifiConfig:
		return { true, false, false };
	default:
		return { true, false, false };
	}
}

BootMode
bootModeAfterPortalSuccess(BootMode currentMode)
{
	switch (currentMode) {
	case BootMode::ApConfig:
	case BootMode::WifiConfig:
		return BootMode::Normal;
	case BootMode::Normal:
	default:
		return currentMode;
	}
}

BootIntent
bootIntentAfterStaPortalConnectFailure(void)
{
	// STA-only config is only reachable if the saved WiFi creds still work.
	// Falling back to AP config keeps the device recoverable instead of silently
	// returning to normal runtime with no usable config surface.
	return BootIntent::ApConfig;
}

const char *
bootModeToString(BootMode mode)
{
	switch (mode) {
	case BootMode::Normal:
		return "normal";
	case BootMode::ApConfig:
		return "ap_config";
	case BootMode::WifiConfig:
		return "wifi_config";
	default:
		return "normal";
	}
}

BootMode
bootModeFromString(const char *value)
{
	if (value == nullptr || *value == '\0') {
		return BootMode::Normal;
	}
	if (strcmp(value, "normal") == 0) {
		return BootMode::Normal;
	}
	if (strcmp(value, "ap_config") == 0) {
		return BootMode::ApConfig;
	}
	if (strcmp(value, "wifi_config") == 0) {
		return BootMode::WifiConfig;
	}
	return BootMode::Normal;
}

const char *
bootIntentToString(BootIntent intent)
{
	switch (intent) {
	case BootIntent::Normal:
		return "normal";
	case BootIntent::ApConfig:
		return "ap_config";
	case BootIntent::WifiConfig:
		return "wifi_config";
	case BootIntent::Ota:
		return "ota";
	default:
		return "normal";
	}
}

BootIntent
bootIntentFromString(const char *value)
{
	if (value == nullptr || *value == '\0') {
		return BootIntent::Normal;
	}
	if (strcmp(value, "normal") == 0) {
		return BootIntent::Normal;
	}
	if (strcmp(value, "ap_config") == 0) {
		return BootIntent::ApConfig;
	}
	if (strcmp(value, "wifi_config") == 0) {
		return BootIntent::WifiConfig;
	}
	if (strcmp(value, "ota") == 0) {
		return BootIntent::Ota;
	}
	return BootIntent::Normal;
}
