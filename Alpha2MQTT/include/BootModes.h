#pragma once

#include <cstdint>

enum class BootMode : uint8_t {
	Normal = 0,
	ApConfig = 1,
	WifiConfig = 2,
};

struct SubsystemStates {
	bool mqttEnabled;
	bool inverterEnabled;
	bool portalEnabled;
	bool portalUsesSta;
};

SubsystemStates decideSubsystems(BootMode mode);
