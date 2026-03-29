// Purpose: Define the atomic dispatch request contract and pure helper logic.
// Responsibilities: Parse MQTT payloads, validate mode-specific fields, map
// API requests onto AlphaESS dispatch registers, and compare register readback.
// Invariants: No Arduino dependencies, no dynamic allocation, and status
// strings remain short human-readable messages suitable for HA display.
#pragma once

#include <cstddef>
#include <cstdint>

#include "Definitions.h"
#include "DispatchTiming.h"

enum class DispatchRequestMode : uint8_t {
	BatteryOnlyChargesFromPv = 0,
	StateOfChargeControl,
	LoadFollowing,
	MaximiseOutput,
	NormalMode,
	OptimiseConsumption,
	MaximiseConsumption,
	Unknown
};

struct DispatchRequestPayload {
	DispatchRequestMode mode = DispatchRequestMode::Unknown;
	bool hasPower = false;
	int32_t powerW = 0;
	bool hasSoc = false;
	uint32_t socPercent = 0;
	bool hasDuration = false;
	uint32_t durationS = 0;
};

struct DispatchRequestPlan {
	DispatchRequestMode mode = DispatchRequestMode::Unknown;
	bool stop = false;
	uint16_t dispatchMode = DISPATCH_MODE_NORMAL_MODE;
	int32_t dispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET;
	uint16_t dispatchSocRaw = 0;
	uint32_t dispatchTimeRaw = 0;
	bool matchStartStop = false;
	bool matchMode = false;
	bool matchPower = false;
	bool matchSoc = false;
	bool matchTime = false;
};

struct DispatchRegisterReadback {
	uint16_t dispatchStart = DISPATCH_START_STOP;
	uint16_t dispatchMode = DISPATCH_MODE_NORMAL_MODE;
	int32_t dispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET;
	uint16_t dispatchSocRaw = 0;
	uint32_t dispatchTimeRaw = 0;
};

struct DispatchReconnectResetPlan {
	bool clearStatus = true;
	bool clearPendingRequest = true;
	bool clearPendingPayload = true;
	bool clearInFlightState = false;
	bool clearQueuedTimestamp = false;
};

const char *dispatchRequestModeApiName(DispatchRequestMode mode);
bool lookupDispatchRequestMode(const char *name, DispatchRequestMode &mode);
bool parseDispatchRequestPayload(const char *payload,
                                 DispatchRequestPayload &out,
                                 char *error,
                                 size_t errorSize);
bool buildDispatchRequestPlan(const DispatchRequestPayload &payload,
                              DispatchRequestPlan &out,
                              char *error,
                              size_t errorSize);
uint32_t dispatchAcceptedDurationSeconds(const DispatchRequestPayload &payload,
                                         const DispatchRequestPlan &plan);
bool dispatchRequestReadbackMatches(const DispatchRequestPlan &plan,
                                    const DispatchRegisterReadback &readback,
                                    char *error,
                                    size_t errorSize);
DispatchReconnectResetPlan dispatchReconnectResetPlan(bool inFlight);
bool dispatchRequestShouldRejectNewRequest(bool pendingRequest, bool inFlight);
bool dispatchRequestStatusShouldPublish(const char *status);
