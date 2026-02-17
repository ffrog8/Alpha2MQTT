#include <doctest/doctest.h>

#include "BucketScheduler.h"

namespace {

static bool needsSnapshotFromTable(size_t idx)
{
	static const bool kNeeds[] = { false, true, false, true, false };
	return idx < (sizeof(kNeeds) / sizeof(kNeeds[0])) ? kNeeds[idx] : false;
}

} // namespace

TEST_CASE("bucket membership assigns entities exclusively by effective frequency")
{
	MqttEntityRuntime rt[5]{};
	rt[0].bucketId = BucketId::TenSec;
	rt[1].bucketId = BucketId::OneMin;
	rt[2].bucketId = BucketId::FiveMin;
	rt[3].bucketId = BucketId::OneMin;
	rt[4].bucketId = BucketId::Disabled;

	uint16_t tenSec[5]{};
	uint16_t oneMin[5]{};
	uint16_t fiveMin[5]{};
	uint16_t oneHour[5]{};
	uint16_t oneDay[5]{};
	uint16_t user[5]{};

	BucketMembership m = buildBucketMembership(rt, 5, tenSec, oneMin, fiveMin, oneHour, oneDay, user,
	                                           needsSnapshotFromTable);

	CHECK(m.tenSecCount == 1);
	CHECK(m.oneMinCount == 2);
	CHECK(m.fiveMinCount == 1);
	CHECK(m.oneHourCount == 0);
	CHECK(m.oneDayCount == 0);

	bool seen[5] = { false, false, false, false, false };
	for (size_t i = 0; i < m.tenSecCount; ++i) {
		seen[tenSec[i]] = true;
	}
	for (size_t i = 0; i < m.oneMinCount; ++i) {
		seen[oneMin[i]] = true;
	}
	for (size_t i = 0; i < m.fiveMinCount; ++i) {
		seen[fiveMin[i]] = true;
	}
	for (size_t i = 0; i < m.oneHourCount; ++i) {
		seen[oneHour[i]] = true;
	}
	for (size_t i = 0; i < m.oneDayCount; ++i) {
		seen[oneDay[i]] = true;
	}
	for (size_t i = 0; i < m.userCount; ++i) {
		seen[user[i]] = true;
	}

	// Entities with an enabled frequency appear exactly once; disabled does not appear.
	CHECK(seen[0]);
	CHECK(seen[1]);
	CHECK(seen[2]);
	CHECK(seen[3]);
	CHECK_FALSE(seen[4]);
}

TEST_CASE("bucket membership tracks whether a bucket contains ESS snapshot entities")
{
	MqttEntityRuntime rt[4]{};
	rt[0].bucketId = BucketId::OneMin;   // needs snapshot (table idx 0 -> false)
	rt[1].bucketId = BucketId::OneMin;   // needs snapshot (table idx 1 -> true)
	rt[2].bucketId = BucketId::TenSec;   // idx 2 -> false
	rt[3].bucketId = BucketId::TenSec;   // idx 3 -> true

	uint16_t tenSec[4]{};
	uint16_t oneMin[4]{};
	uint16_t fiveMin[4]{};
	uint16_t oneHour[4]{};
	uint16_t oneDay[4]{};
	uint16_t user[4]{};

	BucketMembership m = buildBucketMembership(rt, 4, tenSec, oneMin, fiveMin, oneHour, oneDay, user,
	                                           needsSnapshotFromTable);

	CHECK(m.oneMinHasEssSnapshot);
	CHECK(m.tenSecHasEssSnapshot);
	CHECK_FALSE(m.fiveMinHasEssSnapshot);
	CHECK_FALSE(m.oneHourHasEssSnapshot);
	CHECK_FALSE(m.oneDayHasEssSnapshot);
}

TEST_CASE("bucket helpers: snapshot-dependent entities are skipped when snapshot failed")
{
	CHECK(shouldPublishEntityForBucket(false, false));
	CHECK(shouldPublishEntityForBucket(false, true));
	CHECK(shouldPublishEntityForBucket(true, true));
	CHECK_FALSE(shouldPublishEntityForBucket(true, false));
}

TEST_CASE("bucket helpers: dispatch runs only when snapshot succeeded")
{
	CHECK(shouldRunDispatchForTenSecBucket(true));
	CHECK_FALSE(shouldRunDispatchForTenSecBucket(false));
}

TEST_CASE("bucket helpers: ten second cadence always requires snapshot")
{
	CHECK(tenSecBucketRequiresSnapshot());
}

TEST_CASE("bucket helpers: inverter not ready still allows non-snapshot publishes")
{
	const bool snapshotOkThisBucket = snapshotPrereqSatisfiedForBucket(true, true, false, false);
	CHECK_FALSE(snapshotOkThisBucket);
	CHECK(shouldPublishEntityForBucket(false, snapshotOkThisBucket));
	CHECK_FALSE(shouldPublishEntityForBucket(true, snapshotOkThisBucket));
	CHECK_FALSE(shouldRunDispatchForTenSecPass(true, snapshotOkThisBucket, false));
}

TEST_CASE("bucket helpers: snapshot failure skips only snapshot-dependent entities")
{
	bool snapshotAttemptedThisPass = false;
	CHECK(shouldAttemptEssSnapshotRefreshForBucket(true, true, true, snapshotAttemptedThisPass));
	snapshotAttemptedThisPass = true;
	CHECK_FALSE(shouldAttemptEssSnapshotRefreshForBucket(true, true, true, snapshotAttemptedThisPass));

	const bool snapshotOkThisBucket = snapshotPrereqSatisfiedForBucket(true, true, true, false);
	CHECK_FALSE(snapshotOkThisBucket);
	CHECK(shouldPublishEntityForBucket(false, snapshotOkThisBucket));
	CHECK_FALSE(shouldPublishEntityForBucket(true, snapshotOkThisBucket));
	CHECK_FALSE(shouldRunDispatchForTenSecPass(true, snapshotOkThisBucket, false));
}

TEST_CASE("bucket helpers: snapshot success publishes snapshot entities and dispatch runs once")
{
	const bool snapshotOkThisBucket = snapshotPrereqSatisfiedForBucket(true, true, true, true);
	CHECK(snapshotOkThisBucket);
	CHECK(shouldPublishEntityForBucket(false, snapshotOkThisBucket));
	CHECK(shouldPublishEntityForBucket(true, snapshotOkThisBucket));
	CHECK(shouldRunDispatchForTenSecPass(true, snapshotOkThisBucket, false));
	CHECK_FALSE(shouldRunDispatchForTenSecPass(true, snapshotOkThisBucket, true));
}
