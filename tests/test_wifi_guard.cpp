#include "doctest/doctest.h"

#include "WifiGuard.h"

TEST_CASE("wifi guard scan decision")
{
	CHECK_FALSE(shouldStartWifiScan(BootMode::Normal));
	CHECK(shouldStartWifiScan(BootMode::ApConfig));
	CHECK(shouldStartWifiScan(BootMode::WifiConfig));
}
