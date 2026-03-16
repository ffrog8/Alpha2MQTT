#pragma once

#include <cstdint>

enum class BootMode : uint8_t {
	Normal = 0,
	ApConfig = 1,
	WifiConfig = 2,
};

enum class BootIntent : uint8_t {
	Normal = 0,
	ApConfig = 1,
	WifiConfig = 2,
	Ota = 3,
};

struct SubsystemStates {
	bool mqttEnabled;
	bool inverterEnabled;
	bool portalEnabled;
	bool portalUsesSta;
};

struct SubsystemPlan {
	bool wifiSta;
	bool mqtt;
	bool inverter;
};

SubsystemStates decideSubsystems(BootMode mode);
SubsystemPlan planForBootMode(BootMode mode);
BootMode bootModeAfterPortalSuccess(BootMode currentMode);
BootIntent bootIntentAfterStaPortalConnectFailure(void);
BootMode bootModeForIntent(BootIntent intent, BootMode currentMode);
const char *bootModeToString(BootMode mode);
BootMode bootModeFromString(const char *value);
const char *bootIntentToString(BootIntent intent);
BootIntent bootIntentFromString(const char *value);
