// Purpose: Build per-frequency publish buckets at runtime without scanning all entities each loop.
// Invariants: Each entity belongs to at most one bucket (based on its effective frequency).
// Notes: This is pure logic (no Arduino deps) so it can be unit tested on host.
#pragma once

#include <cstddef>
#include <cstdint>

#include "MqttEntities.h"

BucketId bucketIdFromString(const char *value);
const char *bucketIdToString(BucketId bucket);
const char *bucketIdToProfileString(BucketId bucket);
BucketId bucketIdFromFreq(mqttUpdateFreq freq);
mqttUpdateFreq bucketIdToFreq(BucketId bucket);
uint32_t bucketIntervalMs(BucketId bucket, uint32_t userIntervalMs);
uint32_t bucketBudgetMs(BucketId bucket, uint32_t userIntervalMs, uint32_t maxBudgetMs);
int bucketOrdinal(BucketId bucket);

inline bool shouldPublishEntityForBucket(bool entityNeedsEssSnapshot, bool essSnapshotOk)
{
	return !entityNeedsEssSnapshot || essSnapshotOk;
}

template <typename NeedsSnapshotByIndexFn, typename PublishByIndexFn>
inline size_t publishBucketMembers(const uint16_t *members,
                                   size_t memberCount,
                                   bool essSnapshotOk,
                                   NeedsSnapshotByIndexFn needsSnapshotByIndex,
                                   PublishByIndexFn publishByIndex)
{
	size_t publishedCount = 0;
	for (size_t n = 0; n < memberCount; ++n) {
		const size_t idx = members[n];
		if (!shouldPublishEntityForBucket(needsSnapshotByIndex(idx), essSnapshotOk)) {
			continue;
		}
		publishByIndex(idx);
		++publishedCount;
	}
	return publishedCount;
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

BucketMembership buildBucketMembership(const BucketId *buckets,
                                       size_t entityCount,
                                       uint16_t *membersTenSec,
                                       uint16_t *membersOneMin,
                                       uint16_t *membersFiveMin,
                                       uint16_t *membersOneHour,
                                       uint16_t *membersOneDay,
                                       uint16_t *membersUser,
                                       NeedsEssSnapshotFn needsEssSnapshot);
