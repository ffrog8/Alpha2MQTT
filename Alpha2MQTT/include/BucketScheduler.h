// Purpose: Build per-frequency publish buckets at runtime without scanning all entities each loop.
// Invariants: Each entity belongs to at most one bucket (based on its effective frequency).
// Notes: This is pure logic (no Arduino deps) so it can be unit tested on host.
#pragma once

#include <cstddef>
#include <cstdint>

#include "MqttEntities.h"

inline bool shouldPublishEntityForBucket(bool entityNeedsEssSnapshot, bool essSnapshotOk)
{
	return !entityNeedsEssSnapshot || essSnapshotOk;
}

inline bool shouldRunDispatchForTenSecBucket(bool essSnapshotOk)
{
	return essSnapshotOk;
}

struct BucketMembership {
	size_t tenSecCount;
	size_t oneMinCount;
	size_t fiveMinCount;
	size_t oneHourCount;
	size_t oneDayCount;
	bool tenSecHasEssSnapshot;
	bool oneMinHasEssSnapshot;
	bool fiveMinHasEssSnapshot;
	bool oneHourHasEssSnapshot;
	bool oneDayHasEssSnapshot;
};

using NeedsEssSnapshotFn = bool (*)(size_t idx);

BucketMembership buildBucketMembership(const MqttEntityRuntime *rt,
                                      size_t entityCount,
                                      uint16_t *membersTenSec,
                                      uint16_t *membersOneMin,
                                      uint16_t *membersFiveMin,
                                      uint16_t *membersOneHour,
                                      uint16_t *membersOneDay,
                                      NeedsEssSnapshotFn needsEssSnapshot);
