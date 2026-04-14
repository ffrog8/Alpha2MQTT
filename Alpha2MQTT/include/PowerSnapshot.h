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

struct PowerSnapshotBuildMinuteBucket {
	uint32_t minuteId = UINT32_MAX;
	uint16_t minMs = 0;
	uint16_t maxMs = 0;
	uint32_t sumMs = 0;
	uint16_t count = 0;
};

struct PowerSnapshotBuildWindowStats {
	bool hasData = false;
	uint16_t minMs = 0;
	uint16_t maxMs = 0;
	uint16_t avgMs = 0;
	uint32_t sumMs = 0;
	uint16_t count = 0;
};

constexpr float kPvVoltageCurrentMultiplier = 0.1f;
constexpr uint32_t kPowerSnapshotBuildBucketMinuteMs = 60000UL;
constexpr size_t kPowerSnapshotBuildMinuteBucketCount = 15;

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

inline uint32_t
powerSnapshotBuildMinuteId(uint32_t nowMs)
{
	return nowMs / kPowerSnapshotBuildBucketMinuteMs;
}

inline void
clearPowerSnapshotBuildMinuteBucket(PowerSnapshotBuildMinuteBucket &bucket)
{
	bucket.minuteId = UINT32_MAX;
	bucket.minMs = 0;
	bucket.maxMs = 0;
	bucket.sumMs = 0;
	bucket.count = 0;
}

inline void
resetPowerSnapshotBuildMinuteBuckets(PowerSnapshotBuildMinuteBucket *buckets, size_t bucketCount)
{
	if (buckets == nullptr) {
		return;
	}
	for (size_t i = 0; i < bucketCount; ++i) {
		clearPowerSnapshotBuildMinuteBucket(buckets[i]);
	}
}

inline void
recordPowerSnapshotBuildMinuteSample(PowerSnapshotBuildMinuteBucket *buckets,
                                     size_t bucketCount,
                                     uint32_t nowMs,
                                     uint32_t buildMs)
{
	if (buckets == nullptr || bucketCount == 0) {
		return;
	}
	const uint32_t minuteId = powerSnapshotBuildMinuteId(nowMs);
	PowerSnapshotBuildMinuteBucket &bucket = buckets[minuteId % bucketCount];
	if (bucket.minuteId != minuteId) {
		clearPowerSnapshotBuildMinuteBucket(bucket);
		bucket.minuteId = minuteId;
	}

	const uint16_t clampedBuildMs =
		(buildMs > static_cast<uint32_t>(UINT16_MAX)) ? UINT16_MAX : static_cast<uint16_t>(buildMs);
	if (bucket.count == 0) {
		bucket.minMs = clampedBuildMs;
		bucket.maxMs = clampedBuildMs;
		bucket.sumMs = clampedBuildMs;
		bucket.count = 1;
		return;
	}

	if (clampedBuildMs < bucket.minMs) {
		bucket.minMs = clampedBuildMs;
	}
	if (clampedBuildMs > bucket.maxMs) {
		bucket.maxMs = clampedBuildMs;
	}
	bucket.sumMs += clampedBuildMs;
	if (bucket.count < UINT16_MAX) {
		bucket.count++;
	}
}

inline PowerSnapshotBuildWindowStats
aggregatePowerSnapshotBuildWindow(const PowerSnapshotBuildMinuteBucket *buckets,
                                  size_t bucketCount,
                                  uint32_t nowMs,
                                  uint8_t windowMinutes)
{
	PowerSnapshotBuildWindowStats stats{};
	if (buckets == nullptr || bucketCount == 0 || windowMinutes == 0) {
		return stats;
	}

	const uint32_t currentMinute = powerSnapshotBuildMinuteId(nowMs);
	for (size_t i = 0; i < bucketCount; ++i) {
		const PowerSnapshotBuildMinuteBucket &bucket = buckets[i];
		if (bucket.count == 0 || bucket.minuteId == UINT32_MAX) {
			continue;
		}
		const uint32_t ageMinutes = currentMinute - bucket.minuteId;
		if (ageMinutes >= static_cast<uint32_t>(windowMinutes)) {
			continue;
		}
		if (!stats.hasData) {
			stats.hasData = true;
			stats.minMs = bucket.minMs;
			stats.maxMs = bucket.maxMs;
		} else {
			if (bucket.minMs < stats.minMs) {
				stats.minMs = bucket.minMs;
			}
			if (bucket.maxMs > stats.maxMs) {
				stats.maxMs = bucket.maxMs;
			}
		}
		stats.sumMs += bucket.sumMs;
		if (static_cast<uint32_t>(stats.count) + static_cast<uint32_t>(bucket.count) > UINT16_MAX) {
			stats.count = UINT16_MAX;
		} else {
			stats.count = static_cast<uint16_t>(stats.count + bucket.count);
		}
	}

	if (!stats.hasData || stats.count == 0) {
		return stats;
	}
	const uint32_t avgMs = stats.sumMs / stats.count;
	stats.avgMs = (avgMs > static_cast<uint32_t>(UINT16_MAX)) ? UINT16_MAX : static_cast<uint16_t>(avgMs);
	return stats;
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
