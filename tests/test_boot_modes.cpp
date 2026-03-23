#include "doctest/doctest.h"

#include <string>

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

TEST_CASE("STA-only portal failure falls back to AP config")
{
	CHECK(bootIntentAfterStaPortalConnectFailure() == BootIntent::ApConfig);
}

TEST_CASE("initial wifi failure only falls back to AP for portal-applied config")
{
	CHECK(initialWifiFailureAction(BootMode::Normal, BootIntent::Normal) ==
	      InitialWifiFailureAction::ContinueReconnect);
	CHECK(initialWifiFailureAction(BootMode::ApConfig, BootIntent::Normal) ==
	      InitialWifiFailureAction::ContinueReconnect);
	CHECK(initialWifiFailureAction(BootMode::WifiConfig, BootIntent::Normal) ==
	      InitialWifiFailureAction::RebootApConfig);
	CHECK(initialWifiFailureAction(BootMode::Normal, BootIntent::PortalNormal) ==
	      InitialWifiFailureAction::RebootApConfig);
}

TEST_CASE("boot intent maps to the next boot mode")
{
	CHECK(bootModeForIntent(BootIntent::Normal, BootMode::ApConfig) == BootMode::Normal);
	CHECK(bootModeForIntent(BootIntent::PortalNormal, BootMode::ApConfig) == BootMode::Normal);
	CHECK(bootModeForIntent(BootIntent::ApConfig, BootMode::Normal) == BootMode::ApConfig);
	CHECK(bootModeForIntent(BootIntent::WifiConfig, BootMode::Normal) == BootMode::WifiConfig);
	CHECK(bootModeForIntent(BootIntent::Ota, BootMode::WifiConfig) == BootMode::WifiConfig);
}

TEST_CASE("boot intent strings round-trip portal normal")
{
	CHECK(bootIntentFromString("portal_normal") == BootIntent::PortalNormal);
	CHECK(std::string(bootIntentToString(BootIntent::PortalNormal)) == "portal_normal");
}
