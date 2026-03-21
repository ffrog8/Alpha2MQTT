#include "doctest/doctest.h"

#include "WifiGuard.h"

TEST_CASE("wifi guard scan decision")
{
	CHECK_FALSE(shouldStartWifiScan(BootMode::Normal));
	CHECK(shouldStartWifiScan(BootMode::ApConfig));
	CHECK(shouldStartWifiScan(BootMode::WifiConfig));
}

TEST_CASE("wifi guard always neutralizes running async scans")
{
	CHECK(classifyWifiScanGuard(kWifiScanRunningState) == WifiScanGuardAction::RebindNoopCallback);
	CHECK(classifyWifiScanGuard(kWifiScanFailedState) == WifiScanGuardAction::DeleteResults);
	CHECK(classifyWifiScanGuard(3) == WifiScanGuardAction::DeleteResults);
	CHECK(classifyWifiScanGuard(-3) == WifiScanGuardAction::None);
}
