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

TEST_CASE("boot mode plan selects subsystems")
{
	SubsystemPlan normal = planForBootMode(BootMode::Normal);
	CHECK(normal.wifiSta);
	CHECK(normal.mqtt);
	CHECK(normal.inverter);

	SubsystemPlan apConfig = planForBootMode(BootMode::ApConfig);
	CHECK(apConfig.wifiSta);
	CHECK_FALSE(apConfig.mqtt);
	CHECK_FALSE(apConfig.inverter);

	SubsystemPlan wifiConfig = planForBootMode(BootMode::WifiConfig);
	CHECK(wifiConfig.wifiSta);
	CHECK_FALSE(wifiConfig.mqtt);
	CHECK_FALSE(wifiConfig.inverter);
}

TEST_CASE("boot mode resets to normal after portal success")
{
	CHECK(bootModeAfterPortalSuccess(BootMode::ApConfig) == BootMode::Normal);
	CHECK(bootModeAfterPortalSuccess(BootMode::WifiConfig) == BootMode::Normal);
	CHECK(bootModeAfterPortalSuccess(BootMode::Normal) == BootMode::Normal);
}
