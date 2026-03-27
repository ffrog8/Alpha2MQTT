// Purpose: Host-testable scheduler guard for background inverter polling.
#include "../include/SchedulerReadPolicy.h"

bool
shouldSkipScheduledEntityRead(DiscoveryDeviceScope scope,
                              bool inverterReady,
                              bool inverterSerialKnown)
{
	return scope == DiscoveryDeviceScope::Inverter && (!inverterReady || !inverterSerialKnown);
}
