// Purpose: Lock down the scheduler guard that prevents background inverter reads before readiness.
#include "doctest/doctest.h"

#include "SchedulerReadPolicy.h"

TEST_CASE("scheduler read policy: controller entities are never blocked by inverter readiness")
{
	CHECK_FALSE(shouldSkipScheduledEntityRead(DiscoveryDeviceScope::Controller, false, false));
	CHECK_FALSE(shouldSkipScheduledEntityRead(DiscoveryDeviceScope::Controller, false, true));
	CHECK_FALSE(shouldSkipScheduledEntityRead(DiscoveryDeviceScope::Controller, true, false));
	CHECK_FALSE(shouldSkipScheduledEntityRead(DiscoveryDeviceScope::Controller, true, true));
}

TEST_CASE("scheduler read policy: inverter entities stay blocked until both readiness signals are live")
{
	CHECK(shouldSkipScheduledEntityRead(DiscoveryDeviceScope::Inverter, false, false));
	CHECK(shouldSkipScheduledEntityRead(DiscoveryDeviceScope::Inverter, false, true));
	CHECK(shouldSkipScheduledEntityRead(DiscoveryDeviceScope::Inverter, true, false));
	CHECK_FALSE(shouldSkipScheduledEntityRead(DiscoveryDeviceScope::Inverter, true, true));
}
