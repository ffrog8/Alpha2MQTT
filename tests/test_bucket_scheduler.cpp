#include <array>

#include <doctest/doctest.h>

#include "BucketScheduler.h"
#include "Scheduler.h"

namespace {

static bool needsSnapshotFromTable(size_t idx)
{
	static const bool kNeeds[] = { false, true, false, true, false };
	return idx < (sizeof(kNeeds) / sizeof(kNeeds[0])) ? kNeeds[idx] : false;
}

} // namespace

TEST_CASE("bucket membership assigns entities exclusively by effective frequency")
{
	BucketId buckets[5] = {
		BucketId::TenSec,
		BucketId::OneMin,
		BucketId::FiveMin,
		BucketId::OneMin,
		BucketId::Disabled
	};

	uint16_t tenSec[5]{};
	uint16_t oneMin[5]{};
	uint16_t fiveMin[5]{};
	uint16_t oneHour[5]{};
	uint16_t oneDay[5]{};
	uint16_t user[5]{};

	BucketMembership m = buildBucketMembership(buckets, 5, tenSec, oneMin, fiveMin, oneHour, oneDay, user,
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
	BucketId buckets[4] = {
		BucketId::OneMin, // idx 0 -> false
		BucketId::OneMin, // idx 1 -> true
		BucketId::TenSec, // idx 2 -> false
		BucketId::TenSec  // idx 3 -> true
	};

	uint16_t tenSec[4]{};
	uint16_t oneMin[4]{};
	uint16_t fiveMin[4]{};
	uint16_t oneHour[4]{};
	uint16_t oneDay[4]{};
	uint16_t user[4]{};

	BucketMembership m = buildBucketMembership(buckets, 4, tenSec, oneMin, fiveMin, oneHour, oneDay, user,
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

TEST_CASE("bucket member loops: inverter not ready publishes non-snapshot members in each due bucket")
{
	const uint16_t membersTenSec[] = { 0, 1 }; // idx0 non-snapshot, idx1 snapshot
	const uint16_t membersOneMin[] = { 2, 3 }; // idx2 non-snapshot, idx3 snapshot
	const uint16_t membersUser[] = { 4, 1 };   // idx4 non-snapshot, idx1 snapshot
	std::array<size_t, 8> published{};
	size_t publishedCount = 0;

	auto publishRecord = [&](size_t idx) {
		if (publishedCount < published.size()) {
			published[publishedCount++] = idx;
		}
	};

	const bool tenSecSnapshotOk = snapshotPrereqSatisfiedForBucket(true, true, false, false);
	const bool oneMinSnapshotOk = snapshotPrereqSatisfiedForBucket(true, true, false, false);
	const bool userSnapshotOk = snapshotPrereqSatisfiedForBucket(true, true, false, false);
	CHECK_FALSE(tenSecSnapshotOk);
	CHECK_FALSE(oneMinSnapshotOk);
	CHECK_FALSE(userSnapshotOk);

	const size_t tenSecPublished = publishBucketMembers(
		membersTenSec, 2, tenSecSnapshotOk, needsSnapshotFromTable, publishRecord);
	const size_t oneMinPublished = publishBucketMembers(
		membersOneMin, 2, oneMinSnapshotOk, needsSnapshotFromTable, publishRecord);
	const size_t userPublished = publishBucketMembers(
		membersUser, 2, userSnapshotOk, needsSnapshotFromTable, publishRecord);

	CHECK(tenSecPublished == 1);
	CHECK(oneMinPublished == 1);
	CHECK(userPublished == 1);
	CHECK(publishedCount == 3);
	CHECK(published[0] == 0);
	CHECK(published[1] == 2);
	CHECK(published[2] == 4);
}

TEST_CASE("dispatch edge-trigger: two sendData iterations in one 10s interval dispatch once")
{
	uint32_t lastRunTenSeconds = 0;
	size_t dispatchCount = 0;
	const uint32_t kTenSecMs = 10000UL;

	auto runSendDataIteration = [&](uint32_t now, bool snapshotOkThisBucket) {
		const bool dueTenSeconds = shouldRun(now, lastRunTenSeconds, kTenSecMs);
		if (dueTenSeconds) {
			lastRunTenSeconds = now;
		}
		bool dispatchRanThisPass = false;
		if (shouldRunDispatchForTenSecPass(dueTenSeconds, snapshotOkThisBucket, dispatchRanThisPass)) {
			++dispatchCount;
			dispatchRanThisPass = true;
		}
		CHECK_FALSE(shouldRunDispatchForTenSecPass(dueTenSeconds, snapshotOkThisBucket, dispatchRanThisPass));
	};

	runSendDataIteration(10000UL, true); // boundary due
	runSendDataIteration(10001UL, true); // same 10s interval, not due
	CHECK(dispatchCount == 1);

	runSendDataIteration(19999UL, true); // still same boundary window, not due
	CHECK(dispatchCount == 1);

	runSendDataIteration(20000UL, true); // next boundary due
	CHECK(dispatchCount == 2);
}

TEST_CASE("bucket helpers expose interval and capped runtime budget")
{
	CHECK(bucketIntervalMs(BucketId::TenSec, 42000UL) == 10000UL);
	CHECK(bucketIntervalMs(BucketId::User, 42000UL) == 42000UL);
	CHECK(bucketBudgetMs(BucketId::TenSec, 42000UL, 5000UL) == 5000UL);
	CHECK(bucketBudgetMs(BucketId::User, 3000UL, 5000UL) == 3000UL);
	CHECK(bucketBudgetMs(BucketId::Disabled, 3000UL, 5000UL) == 0UL);
}

TEST_CASE("bucket helpers provide stable ordinals for runtime cursor arrays")
{
	CHECK(bucketOrdinal(BucketId::TenSec) == 0);
	CHECK(bucketOrdinal(BucketId::OneMin) == 1);
	CHECK(bucketOrdinal(BucketId::FiveMin) == 2);
	CHECK(bucketOrdinal(BucketId::OneHour) == 3);
	CHECK(bucketOrdinal(BucketId::OneDay) == 4);
	CHECK(bucketOrdinal(BucketId::User) == 5);
	CHECK(bucketOrdinal(BucketId::Disabled) == -1);
}
