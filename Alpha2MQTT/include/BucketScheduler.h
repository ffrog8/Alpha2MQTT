// Purpose: Build per-frequency publish buckets at runtime without scanning all entities each loop.
// Invariants: Each entity belongs to at most one bucket (based on its effective frequency).
// Notes: This is pure logic (no Arduino deps) so it can be unit tested on host.
#pragma once

#include <cstddef>
#include <cstdint>

#include "MqttEntities.h"

BucketId bucketIdFromString(const char *value);
const char *bucketIdToString(BucketId bucket);
BucketId bucketIdFromFreq(mqttUpdateFreq freq);
mqttUpdateFreq bucketIdToFreq(BucketId bucket);

inline bool shouldPublishEntityForBucket(bool entityNeedsEssSnapshot, bool essSnapshotOk)
{
	return !entityNeedsEssSnapshot || essSnapshotOk;
}

inline bool shouldAttemptEssSnapshotRefreshForBucket(bool bucketNeedsEssSnapshot,
                                                     bool inverterEnabled,
                                                     bool inverterReady,
                                                     bool snapshotAttemptedThisPass)
{
	return bucketNeedsEssSnapshot && inverterEnabled && inverterReady && !snapshotAttemptedThisPass;
}

inline bool snapshotPrereqSatisfiedForBucket(bool bucketNeedsEssSnapshot,
                                             bool inverterEnabled,
                                             bool inverterReady,
                                             bool essSnapshotOk)
{
	if (!bucketNeedsEssSnapshot) {
		return true;
	}
	if (!inverterEnabled || !inverterReady) {
		return false;
	}
	return essSnapshotOk;
}

inline bool shouldRunDispatchForTenSecBucket(bool essSnapshotOk)
{
	return essSnapshotOk;
}

inline bool shouldRunDispatchForTenSecPass(bool dueTenSecBucket,
                                           bool essSnapshotOk,
                                           bool dispatchAlreadyRanThisPass)
{
	return dueTenSecBucket && shouldRunDispatchForTenSecBucket(essSnapshotOk) && !dispatchAlreadyRanThisPass;
}

inline bool tenSecBucketRequiresSnapshot(void)
{
	return true;
}

struct BucketMembership {
	size_t tenSecCount;
	size_t oneMinCount;
	size_t fiveMinCount;
	size_t oneHourCount;
	size_t oneDayCount;
	size_t userCount;
	bool tenSecHasEssSnapshot;
	bool oneMinHasEssSnapshot;
	bool fiveMinHasEssSnapshot;
	bool oneHourHasEssSnapshot;
	bool oneDayHasEssSnapshot;
	bool userHasEssSnapshot;
};

using NeedsEssSnapshotFn = bool (*)(size_t idx);

BucketMembership buildBucketMembership(const MqttEntityRuntime *rt,
                                      size_t entityCount,
                                      uint16_t *membersTenSec,
                                      uint16_t *membersOneMin,
                                      uint16_t *membersFiveMin,
                                      uint16_t *membersOneHour,
                                      uint16_t *membersOneDay,
                                      uint16_t *membersUser,
                                      NeedsEssSnapshotFn needsEssSnapshot);
