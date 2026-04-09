// Purpose: Define the source-group, pass-id, and snapshot coordination rules
// shared by the runtime scheduler and host tests.
// Invariants: Source-group cache entries are reusable only within one
// scheduler pass, and snapshot-derived publishes must come from one
// snapshot/pass identity.
#pragma once

#include <cstddef>
#include <cstdint>

#include "Definitions.h"

struct SourceGroupReadMeta {
	uint32_t passId = 0;
	uint32_t readStartedMs = 0;
	uint32_t readCompletedMs = 0;
	bool valid = false;
};

struct EssSnapshotMeta {
	uint32_t snapshotId = 0;
	uint32_t passId = 0;
	uint32_t builtStartedMs = 0;
	uint32_t builtCompletedMs = 0;
	bool valid = false;
};

constexpr float kPvVoltageCurrentMultiplier = 0.1f;

constexpr uint16_t kDispatchBlockStartReg = REG_DISPATCH_RW_DISPATCH_START;
constexpr uint16_t kDispatchBlockRegisterCount = 9;

constexpr uint16_t kPvStringBlockStartReg = REG_INVERTER_HOME_R_PV1_VOLTAGE;
constexpr uint16_t kPvStringBlockRegisterCount =
	static_cast<uint16_t>(REG_INVERTER_HOME_R_PV6_POWER_1 + 1 - REG_INVERTER_HOME_R_PV1_VOLTAGE + 1);
constexpr size_t kPvStringCount = 6;

inline bool
sourceGroupCacheReusableForPass(const SourceGroupReadMeta &meta, uint32_t passId)
{
	return meta.valid && meta.passId == passId;
}

inline bool
snapshotPublishAllowedForPass(const EssSnapshotMeta &meta, uint32_t passId)
{
	return meta.valid && meta.passId == passId && meta.snapshotId == passId;
}

inline void
populateEssSnapshotMeta(EssSnapshotMeta &meta,
                        uint32_t passId,
                        uint32_t builtStartedMs,
                        uint32_t builtCompletedMs,
                        bool valid)
{
	meta.passId = passId;
	meta.snapshotId = passId;
	meta.builtStartedMs = builtStartedMs;
	meta.builtCompletedMs = builtCompletedMs;
	meta.valid = valid;
}

inline float
dispatchSocPercentFromRaw(uint16_t raw)
{
	return raw * DISPATCH_SOC_MULTIPLIER;
}

inline float
pvVoltageCurrentFromRaw(uint16_t raw)
{
	return raw * kPvVoltageCurrentMultiplier;
}

inline bool
isEssPowerSnapshotEntityId(mqttEntityId entityId)
{
	switch (entityId) {
	case mqttEntityId::entityBatPwr:
	case mqttEntityId::entityGridPwr:
	case mqttEntityId::entityPvPwr:
	case mqttEntityId::entityLoadPwr:
		return true;
	default:
		return false;
	}
}

inline bool
shouldQueueDispatchRequest(bool pendingDispatchRequest, bool dispatchInFlight, bool snapshotBuildInProgress)
{
	return pendingDispatchRequest && !dispatchInFlight && snapshotBuildInProgress;
}

inline bool
shouldRejectDispatchRequest(bool pendingDispatchRequest, bool dispatchInFlight, bool snapshotBuildInProgress)
{
	return dispatchInFlight || (pendingDispatchRequest && !snapshotBuildInProgress);
}

inline bool
isDispatchBlockReadKey(uint16_t readKey)
{
	switch (readKey) {
	case REG_DISPATCH_RW_DISPATCH_START:
	case REG_DISPATCH_RW_DISPATCH_MODE:
	case REG_DISPATCH_RW_ACTIVE_POWER_1:
	case REG_DISPATCH_RW_DISPATCH_SOC:
	case REG_DISPATCH_RW_DISPATCH_TIME_1:
		return true;
	default:
		return false;
	}
}

inline bool
isPvStringBlockReadKey(uint16_t readKey)
{
	return readKey >= kPvStringBlockStartReg &&
	       readKey < static_cast<uint16_t>(kPvStringBlockStartReg + kPvStringBlockRegisterCount);
}
