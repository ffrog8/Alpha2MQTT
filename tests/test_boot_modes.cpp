#include "doctest/doctest.h"

#include "BootModes.h"

TEST_CASE("boot mode subsystem gating")
{
	SubsystemStates normal = decideSubsystems(BootMode::Normal);
	CHECK(normal.mqttEnabled);
	CHECK(normal.inverterEnabled);
	CHECK_FALSE(normal.portalEnabled);

	SubsystemStates apConfig = decideSubsystems(BootMode::ApConfig);
	CHECK_FALSE(apConfig.mqttEnabled);
	CHECK_FALSE(apConfig.inverterEnabled);
	CHECK(apConfig.portalEnabled);
	CHECK_FALSE(apConfig.portalUsesSta);

	SubsystemStates wifiConfig = decideSubsystems(BootMode::WifiConfig);
	CHECK_FALSE(wifiConfig.mqttEnabled);
	CHECK_FALSE(wifiConfig.inverterEnabled);
	CHECK(wifiConfig.portalEnabled);
	CHECK(wifiConfig.portalUsesSta);
}
