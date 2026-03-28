/*
 * SchedulerReadPolicy keeps background poll gating pure and host-testable.
 * It answers whether scheduled polling may touch inverter-scoped entities,
 * while explicit manual reads and command handlers remain outside this policy.
 */
#pragma once

#include "DiscoveryModel.h"

bool shouldSkipScheduledEntityRead(DiscoveryDeviceScope scope,
                                   bool inverterReady,
                                   bool inverterSerialKnown);
