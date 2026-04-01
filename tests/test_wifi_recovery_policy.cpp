#include "doctest/doctest.h"

#include "WifiRecoveryPolicy.h"

TEST_CASE("wifi recovery timing defaults stay production sized")
{
	const WifiRecoveryTiming timing = wifiRecoveryTiming();
	CHECK(timing.bootValidationMs == 30000U);
	CHECK(timing.runtimeValidationMs == 90000U);
	CHECK(timing.apIdleTimeoutMs == 300000U);
}

TEST_CASE("wifi recovery class only escalates invalid config signals")
{
	WifiFailureSignals connected;
	connected.connected = true;
	CHECK(classifyWifiFailure(connected) == WifiFailureClass::Unknown);

	WifiFailureSignals missingSsid;
	missingSsid.missingSsid = true;
	CHECK(classifyWifiFailure(missingSsid) == WifiFailureClass::InvalidConfig);

	WifiFailureSignals connectFailed;
	connectFailed.connectFailed = true;
	CHECK(classifyWifiFailure(connectFailed) == WifiFailureClass::InvalidConfig);

	WifiFailureSignals authFailed;
	authFailed.authFailed = true;
	CHECK(classifyWifiFailure(authFailed) == WifiFailureClass::InvalidConfig);

	WifiFailureSignals unknown;
	CHECK(classifyWifiFailure(unknown) == WifiFailureClass::Unknown);
}

TEST_CASE("initial invalid wifi failure falls back to AP from normal boots")
{
	CHECK(shouldRebootApOnInitialWifiFailure(BootMode::Normal, WifiFailureClass::InvalidConfig));
	CHECK(shouldRebootApOnInitialWifiFailure(BootMode::WifiConfig, WifiFailureClass::InvalidConfig));
	CHECK_FALSE(shouldRebootApOnInitialWifiFailure(BootMode::ApConfig, WifiFailureClass::InvalidConfig));
	CHECK_FALSE(shouldRebootApOnInitialWifiFailure(BootMode::Normal, WifiFailureClass::Unknown));
}

TEST_CASE("runtime invalid wifi failure only escalates normal mode with stored creds")
{
	CHECK(shouldRebootApOnRuntimeWifiFailure(
		BootMode::Normal, true, WifiFailureClass::InvalidConfig));
	CHECK_FALSE(shouldRebootApOnRuntimeWifiFailure(
		BootMode::Normal, false, WifiFailureClass::InvalidConfig));
	CHECK_FALSE(shouldRebootApOnRuntimeWifiFailure(
		BootMode::WifiConfig, true, WifiFailureClass::InvalidConfig));
	CHECK_FALSE(shouldRebootApOnRuntimeWifiFailure(
		BootMode::Normal, true, WifiFailureClass::Unknown));
}

TEST_CASE("sta portal only falls back to AP on invalid saved wifi")
{
	CHECK(shouldRebootApAfterStaPortalConnectFailure(true, WifiFailureClass::InvalidConfig));
	CHECK_FALSE(shouldRebootApAfterStaPortalConnectFailure(true, WifiFailureClass::Unknown));
	CHECK_FALSE(shouldRebootApAfterStaPortalConnectFailure(false, WifiFailureClass::InvalidConfig));
}

TEST_CASE("AP idle timeout only reboots to normal when wifi exists")
{
	const WifiRecoveryTiming timing = wifiRecoveryTiming();
	CHECK_FALSE(shouldRebootNormalAfterApIdle(false, timing.apIdleTimeoutMs));
	CHECK_FALSE(shouldRebootNormalAfterApIdle(true, timing.apIdleTimeoutMs - 1));
	CHECK(shouldRebootNormalAfterApIdle(true, timing.apIdleTimeoutMs));
}
