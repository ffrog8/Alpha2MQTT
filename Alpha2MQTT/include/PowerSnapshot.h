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
constexpr size_t kPowerSnapshotDiagSubreadCount = 4;
constexpr uint16_t kPowerSnapshotDiagTotalSlowThresholdQ10 = 50;
constexpr uint16_t kPowerSnapshotDiagSubreadSlowThresholdQ10 = 20;
constexpr int32_t kPowerSnapshotDiagLowLoadThresholdW = 50;
constexpr uint8_t kPowerSnapshotConfirmMaxSamples = 3;

enum class PowerSnapshotDiagSubreadId : uint8_t {
	Battery = 0,
	Grid = 1,
	PvMeter = 2,
	PvBlock = 3
};

enum PowerSnapshotDiagReasonBits : uint8_t {
	PowerSnapshotDiagReasonNone = 0,
	PowerSnapshotDiagReasonSlowTotal = 1U << 0,
	PowerSnapshotDiagReasonRetry = 1U << 1,
	PowerSnapshotDiagReasonFailure = 1U << 2,
	PowerSnapshotDiagReasonLowLoad = 1U << 3
};

struct PowerSnapshotDiagSubreadRuntime {
	uint16_t totalQ10 = 0;
	uint16_t waitQ10 = 0;
	uint16_t quietQ10 = 0;
	uint8_t attempts = 0;
	uint8_t retries = 0;
	modbusRequestAndResponseStatusValues result = modbusRequestAndResponseStatusValues::preProcessing;
};

struct PowerTupleSnapshot {
	bool valid = false;
	int16_t batteryW = INT16_MAX;
	int32_t gridW = INT32_MAX;
	int32_t pvW = INT32_MAX;
	int32_t loadW = INT32_MAX;
	uint16_t totalQ10 = 0;
	PowerSnapshotDiagSubreadRuntime subreads[kPowerSnapshotDiagSubreadCount]{};
};

struct PowerSnapshotConfirmSummary {
	bool triggered = false;
	uint8_t samples = 0;
	bool accepted = false;
	uint8_t selectedIndex = 0; // 1-based for retained MQTT readability; 0 means no accepted tuple.
};

struct PowerSnapshotDiagEventRuntime {
	bool valid = false;
	uint32_t tsMs = 0;
	uint8_t reasonMask = PowerSnapshotDiagReasonNone;
	uint16_t totalQ10 = 0;
	int32_t loadW = INT32_MAX;
	uint32_t dispatchRequestQueuedMs = 0;
	uint32_t dispatchLastRunMs = 0;
	PowerSnapshotConfirmSummary confirm{};
	PowerSnapshotDiagSubreadRuntime subreads[kPowerSnapshotDiagSubreadCount]{};
};

struct PowerSnapshotDiagSubreadCounters {
	uint16_t slowCount = 0;
	uint16_t retryCount = 0;
	uint16_t timeoutCount = 0;
	uint16_t invalidFrameCount = 0;
	uint16_t maxTotalQ10 = 0;
};

struct PowerSnapshotDiagCountsRuntime {
	uint32_t interestingEventCount = 0;
	uint32_t loadLowEventCount = 0;
	uint32_t confirmTriggered = 0;
	uint32_t confirmResolved = 0;
	uint32_t confirmSkippedPublish = 0;
	PowerSnapshotDiagSubreadCounters subreads[kPowerSnapshotDiagSubreadCount]{};
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

	uint8_t candidates[kPowerSnapshotConfirmMaxSamples] = {0, 0, 0};
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
		// When two tuples survive filtering, bias toward the later one because the
		// inverter is more likely to have settled after the extra read.
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

inline bool
sourceGroupCacheReusableForPass(const SourceGroupReadMeta &meta, uint32_t passId)
{
	return meta.valid && meta.passId == passId;
}

inline uint16_t
quantizeMillisToQ10(uint32_t ms)
{
	constexpr uint32_t kMaxRoundedMs = static_cast<uint32_t>(UINT16_MAX) * 10U - 5U;
	if (ms > kMaxRoundedMs) {
		return UINT16_MAX;
	}
	const uint32_t rounded = (ms + 5U) / 10U;
	return (rounded > static_cast<uint32_t>(UINT16_MAX)) ? UINT16_MAX : static_cast<uint16_t>(rounded);
}

inline const char *
modbusStatusMqttLabel(modbusRequestAndResponseStatusValues result)
{
	switch (result) {
	case modbusRequestAndResponseStatusValues::preProcessing:
		return MODBUS_REQUEST_AND_RESPONSE_PREPROCESSING_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::notHandledRegister:
		return MODBUS_REQUEST_AND_RESPONSE_NOT_HANDLED_REGISTER_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::invalidFrame:
		return MODBUS_REQUEST_AND_RESPONSE_INVALID_FRAME_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::responseTooShort:
		return MODBUS_REQUEST_AND_RESPONSE_RESPONSE_TOO_SHORT_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::noResponse:
		return MODBUS_REQUEST_AND_RESPONSE_NO_RESPONSE_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::noMQTTPayload:
		return MODBUS_REQUEST_AND_RESPONSE_NO_MQTT_PAYLOAD_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::invalidMQTTPayload:
		return MODBUS_REQUEST_AND_RESPONSE_INVALID_MQTT_PAYLOAD_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::writeSingleRegisterSuccess:
		return MODBUS_REQUEST_AND_RESPONSE_WRITE_SINGLE_REGISTER_SUCCESS_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::writeDataRegisterSuccess:
		return MODBUS_REQUEST_AND_RESPONSE_WRITE_DATA_REGISTER_SUCCESS_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::readDataRegisterSuccess:
		return MODBUS_REQUEST_AND_RESPONSE_READ_DATA_REGISTER_SUCCESS_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::slaveError:
		return MODBUS_REQUEST_AND_RESPONSE_ERROR_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::setDischargeSuccess:
		return MODBUS_REQUEST_AND_RESPONSE_SET_DISCHARGE_SUCCESS_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::setChargeSuccess:
		return MODBUS_REQUEST_AND_RESPONSE_SET_CHARGE_SUCCESS_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::setNormalSuccess:
		return MODBUS_REQUEST_AND_RESPONSE_SET_NORMAL_SUCCESS_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::payloadExceededCapacity:
		return MODBUS_REQUEST_AND_RESPONSE_PAYLOAD_EXCEEDED_CAPACITY_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::addedToPayload:
		return MODBUS_REQUEST_AND_RESPONSE_ADDED_TO_PAYLOAD_MQTT_DESC;
	case modbusRequestAndResponseStatusValues::readDataInvalidValue:
		return MODBUS_REQUEST_AND_RESPONSE_READ_DATA_INVALID_VALUE_MQTT_DESC;
	default:
		return MODBUS_REQUEST_AND_RESPONSE_PREPROCESSING_MQTT_DESC;
	}
}

inline bool
modbusStatusIsSuccess(modbusRequestAndResponseStatusValues result)
{
	return result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess ||
	       result == modbusRequestAndResponseStatusValues::writeDataRegisterSuccess ||
	       result == modbusRequestAndResponseStatusValues::writeSingleRegisterSuccess;
}

inline void
capturePowerSnapshotSubreadRuntime(PowerSnapshotDiagSubreadRuntime &target,
                                   uint32_t totalMs,
                                   uint16_t waitQ10,
                                   uint16_t quietQ10,
                                   uint8_t attempts,
                                   uint8_t retries,
                                   modbusRequestAndResponseStatusValues result)
{
	target.totalQ10 = quantizeMillisToQ10(totalMs);
	target.waitQ10 = waitQ10;
	target.quietQ10 = quietQ10;
	target.attempts = attempts;
	target.retries = retries;
	target.result = result;
}

inline void
capturePowerSnapshotCachedSubreadRuntime(PowerSnapshotDiagSubreadRuntime *target,
                                         const SourceGroupReadMeta &meta)
{
	if (target == nullptr) {
		return;
	}
	capturePowerSnapshotSubreadRuntime(*target,
	                                   static_cast<uint32_t>(meta.readCompletedMs - meta.readStartedMs),
	                                   0,
	                                   0,
	                                   1,
	                                   0,
	                                   modbusRequestAndResponseStatusValues::readDataRegisterSuccess);
}

inline uint8_t
computePowerSnapshotDiagReasonMask(const PowerSnapshotDiagSubreadRuntime *subreads,
                                   size_t subreadCount,
                                   uint16_t totalQ10,
                                   int32_t loadW)
{
	uint8_t reasonMask = PowerSnapshotDiagReasonNone;
	if (totalQ10 >= kPowerSnapshotDiagTotalSlowThresholdQ10) {
		reasonMask |= PowerSnapshotDiagReasonSlowTotal;
	}
	for (size_t i = 0; i < subreadCount; ++i) {
		const PowerSnapshotDiagSubreadRuntime &subread = subreads[i];
		if (subread.retries > 0) {
			reasonMask |= PowerSnapshotDiagReasonRetry;
		}
		if (!modbusStatusIsSuccess(subread.result)) {
			reasonMask |= PowerSnapshotDiagReasonFailure;
		}
	}
	if (loadW != INT32_MAX && loadW <= kPowerSnapshotDiagLowLoadThresholdW) {
		reasonMask |= PowerSnapshotDiagReasonLowLoad;
	}
	return reasonMask;
}

inline bool
recordPowerSnapshotDiagCounts(PowerSnapshotDiagCountsRuntime &counts,
                              const PowerSnapshotDiagSubreadRuntime *subreads,
                              size_t subreadCount,
                              uint8_t reasonMask)
{
	bool changed = false;
	if (reasonMask != PowerSnapshotDiagReasonNone) {
		counts.interestingEventCount++;
		changed = true;
	}
	if ((reasonMask & PowerSnapshotDiagReasonLowLoad) != 0) {
		counts.loadLowEventCount++;
		changed = true;
	}
	for (size_t i = 0; i < subreadCount && i < kPowerSnapshotDiagSubreadCount; ++i) {
		PowerSnapshotDiagSubreadCounters &counter = counts.subreads[i];
		const PowerSnapshotDiagSubreadRuntime &subread = subreads[i];
		if (subread.totalQ10 > counter.maxTotalQ10) {
			counter.maxTotalQ10 = subread.totalQ10;
			changed = true;
		}
		if (subread.totalQ10 >= kPowerSnapshotDiagSubreadSlowThresholdQ10) {
			counter.slowCount++;
			changed = true;
		}
		if (subread.retries > 0) {
			counter.retryCount++;
			changed = true;
		}
		if (subread.result == modbusRequestAndResponseStatusValues::noResponse) {
			counter.timeoutCount++;
			changed = true;
		}
		if (subread.result == modbusRequestAndResponseStatusValues::invalidFrame) {
			counter.invalidFrameCount++;
			changed = true;
		}
	}
	return changed;
}

inline bool
snapshotPublishAllowedForPass(const EssSnapshotMeta &meta, uint32_t passId)
{
	return meta.valid && meta.passId == passId && meta.snapshotId == passId;
}

inline void
rearmPowerSnapshotDiagRetainedPublishes(bool hasLastEvent, bool &lastDirty, bool &countsDirty)
{
	countsDirty = true;
	if (hasLastEvent) {
		lastDirty = true;
	}
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
