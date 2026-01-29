#include "../include/BootModes.h"

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
