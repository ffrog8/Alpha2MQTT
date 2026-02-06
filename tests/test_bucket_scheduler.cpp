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
	rt[0].effectiveFreq = mqttUpdateFreq::freqTenSec;
	rt[1].effectiveFreq = mqttUpdateFreq::freqOneMin;
	rt[2].effectiveFreq = mqttUpdateFreq::freqFiveMin;
	rt[3].effectiveFreq = mqttUpdateFreq::freqOneMin;
	rt[4].effectiveFreq = mqttUpdateFreq::freqDisabled;

	uint16_t tenSec[5]{};
	uint16_t oneMin[5]{};
	uint16_t fiveMin[5]{};
	uint16_t oneHour[5]{};
	uint16_t oneDay[5]{};

	BucketMembership m = buildBucketMembership(rt, 5, tenSec, oneMin, fiveMin, oneHour, oneDay, needsSnapshotFromTable);

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
	rt[0].effectiveFreq = mqttUpdateFreq::freqOneMin;   // needs snapshot (table idx 0 -> false)
	rt[1].effectiveFreq = mqttUpdateFreq::freqOneMin;   // needs snapshot (table idx 1 -> true)
	rt[2].effectiveFreq = mqttUpdateFreq::freqTenSec;   // idx 2 -> false
	rt[3].effectiveFreq = mqttUpdateFreq::freqTenSec;   // idx 3 -> true

	uint16_t tenSec[4]{};
	uint16_t oneMin[4]{};
	uint16_t fiveMin[4]{};
	uint16_t oneHour[4]{};
	uint16_t oneDay[4]{};

	BucketMembership m = buildBucketMembership(rt, 4, tenSec, oneMin, fiveMin, oneHour, oneDay, needsSnapshotFromTable);

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
