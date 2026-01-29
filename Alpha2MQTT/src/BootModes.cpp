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
