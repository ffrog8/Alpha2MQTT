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

constexpr size_t kPowerSnapshotDiagSubreadCount = 4;
constexpr uint16_t kPowerSnapshotDiagLowLoadThresholdW = 50;
constexpr uint16_t kPowerSnapshotDiagSlowTotalThresholdQ10 = 50;
constexpr uint16_t kPowerSnapshotDiagSubreadSlowThresholdQ10 = 20;
constexpr uint8_t kPowerSnapshotConfirmMaxSamples = 3;

enum class PowerSnapshotDiagSubread : uint8_t {
	Battery = 0,
	Grid = 1,
	PvMeter = 2,
	PvBlock = 3,
};

enum class PowerSnapshotDiagReason : uint8_t {
	None = 0,
	InvalidRead = 1,
	NegativeLoad = 2,
	LowLoad = 3,
	SlowTotal = 4,
};

struct PowerSnapshotSubreadDiag {
	uint16_t totalQ10 = 0;
	uint16_t waitQ10 = 0;
	uint16_t quietQ10 = 0;
	uint8_t attempts = 0;
	uint8_t retries = 0;
	uint8_t resultCode = static_cast<uint8_t>(modbusRequestAndResponseStatusValues::preProcessing);
};

struct PowerTupleSnapshot {
	bool valid = false;
	int16_t batteryW = INT16_MAX;
	int32_t gridW = INT32_MAX;
	int32_t pvW = INT32_MAX;
	int32_t loadW = INT32_MAX;
	uint16_t totalQ10 = 0;
	PowerSnapshotSubreadDiag subreads[kPowerSnapshotDiagSubreadCount]{};
};

struct PowerSnapshotConfirmSummary {
	bool triggered = false;
	uint8_t samples = 0;
	bool accepted = false;
	uint8_t selectedIndex = 0;
};

struct PowerSnapshotDiagEventRuntime {
	bool valid = false;
	PowerSnapshotDiagReason reason = PowerSnapshotDiagReason::None;
	uint32_t tsMs = 0;
	int32_t triggerLoadW = INT32_MAX;
	int32_t loadW = INT32_MAX;
	uint16_t totalQ10 = 0;
	PowerSnapshotConfirmSummary confirm{};
	PowerSnapshotSubreadDiag subreads[kPowerSnapshotDiagSubreadCount]{};
};

struct PowerSnapshotSubreadCountRuntime {
	uint32_t retryCount = 0;
	uint32_t timeoutCount = 0;
	uint32_t invalidFrameCount = 0;
	uint32_t slowCount = 0;
	uint16_t maxTotalQ10 = 0;
};

struct PowerSnapshotDiagCountsRuntime {
	uint32_t interestingEvents = 0;
	uint32_t invalidReadEvents = 0;
	uint32_t negativeLoadEvents = 0;
	uint32_t lowLoadEvents = 0;
	uint32_t slowTotalEvents = 0;
	uint32_t confirmTriggered = 0;
	uint32_t confirmResolved = 0;
	uint32_t confirmSkippedPublish = 0;
	PowerSnapshotSubreadCountRuntime subreads[kPowerSnapshotDiagSubreadCount]{};
};

inline bool
powerSnapshotLoadNegative(int32_t loadW)
{
	return loadW != INT32_MAX && loadW < 0;
}

inline bool
powerSnapshotLoadLow(int32_t loadW)
{
	return loadW != INT32_MAX && loadW >= 0 && loadW <= kPowerSnapshotDiagLowLoadThresholdW;
}

inline bool
powerSnapshotLoadSuspicious(int32_t loadW)
{
	return loadW != INT32_MAX && loadW <= kPowerSnapshotDiagLowLoadThresholdW;
}

inline bool
powerSnapshotTupleSuspicious(const PowerTupleSnapshot &sample)
{
	return !sample.valid || powerSnapshotLoadSuspicious(sample.loadW);
}

inline PowerSnapshotDiagReason
powerSnapshotDiagReasonForSample(const PowerTupleSnapshot &sample)
{
	if (!sample.valid) {
		return PowerSnapshotDiagReason::InvalidRead;
	}
	if (powerSnapshotLoadNegative(sample.loadW)) {
		return PowerSnapshotDiagReason::NegativeLoad;
	}
	if (powerSnapshotLoadLow(sample.loadW)) {
		return PowerSnapshotDiagReason::LowLoad;
	}
	if (sample.totalQ10 >= kPowerSnapshotDiagSlowTotalThresholdQ10) {
		return PowerSnapshotDiagReason::SlowTotal;
	}
	return PowerSnapshotDiagReason::None;
}

inline bool
powerSnapshotShouldDiscardCandidate(const PowerTupleSnapshot &sample, bool keepOnlyNonNegative, bool keepOnlyAboveLow)
{
	if (!sample.valid) {
		return true;
	}
	if (keepOnlyNonNegative && powerSnapshotLoadNegative(sample.loadW)) {
		return true;
	}
	if (keepOnlyAboveLow && sample.loadW <= kPowerSnapshotDiagLowLoadThresholdW) {
		return true;
	}
	return false;
}

inline bool
selectConfirmedPowerTuple(const PowerTupleSnapshot *samples, uint8_t sampleCount, uint8_t &selectedIndexOut)
{
	selectedIndexOut = 0;
	if (samples == nullptr || sampleCount == 0 || sampleCount > kPowerSnapshotConfirmMaxSamples) {
		return false;
	}

	bool hasNonNegative = false;
	bool hasAboveLow = false;
	for (uint8_t i = 0; i < sampleCount; ++i) {
		if (!samples[i].valid) {
			continue;
		}
		hasNonNegative = hasNonNegative || !powerSnapshotLoadNegative(samples[i].loadW);
		hasAboveLow = hasAboveLow || (samples[i].loadW > kPowerSnapshotDiagLowLoadThresholdW);
	}

	uint8_t candidates[kPowerSnapshotConfirmMaxSamples] = { 0, 0, 0 };
	uint8_t candidateCount = 0;
	for (uint8_t i = 0; i < sampleCount; ++i) {
		if (powerSnapshotShouldDiscardCandidate(samples[i], hasNonNegative, hasAboveLow)) {
			continue;
		}
		candidates[candidateCount++] = i;
	}

	if (candidateCount == 0) {
		return false;
	}
	if (candidateCount == 1) {
		selectedIndexOut = candidates[0];
		return true;
	}
	if (candidateCount == 2) {
		// Two survivors means the third sample was filtered out. Bias toward the later
		// sample because the inverter is more likely to have settled after the extra read.
		selectedIndexOut = candidates[1];
		return true;
	}

	const int32_t load0 = samples[candidates[0]].loadW;
	const int32_t load1 = samples[candidates[1]].loadW;
	const int32_t load2 = samples[candidates[2]].loadW;
	if ((load0 <= load1 && load1 <= load2) || (load2 <= load1 && load1 <= load0)) {
		selectedIndexOut = candidates[1];
		return true;
	}
	if ((load1 <= load0 && load0 <= load2) || (load2 <= load0 && load0 <= load1)) {
		selectedIndexOut = candidates[0];
		return true;
	}
	selectedIndexOut = candidates[2];
	return true;
}
