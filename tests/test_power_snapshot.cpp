#include <doctest/doctest.h>

#include "PowerSnapshot.h"

TEST_CASE("power snapshot helpers reuse source-group cache only in the same pass")
{
	SourceGroupReadMeta meta{};
	meta.valid = true;
	meta.passId = 7;

	CHECK(sourceGroupCacheReusableForPass(meta, 7));
	CHECK_FALSE(sourceGroupCacheReusableForPass(meta, 8));

	meta.valid = false;
	CHECK_FALSE(sourceGroupCacheReusableForPass(meta, 7));
}

TEST_CASE("power snapshot helpers require matching pass identity for snapshot publish")
{
	EssSnapshotMeta meta{};
	meta.valid = true;
	meta.passId = 11;
	meta.snapshotId = 11;

	CHECK(snapshotPublishAllowedForPass(meta, 11));

	meta.snapshotId = 10;
	CHECK_FALSE(snapshotPublishAllowedForPass(meta, 11));

	meta.snapshotId = 11;
	meta.passId = 12;
	CHECK_FALSE(snapshotPublishAllowedForPass(meta, 11));
}

TEST_CASE("power snapshot helpers populate ESS snapshot metadata with pass identity")
{
	EssSnapshotMeta meta{};
	populateEssSnapshotMeta(meta, 19, 1200, 1250, true);

	CHECK(meta.passId == 19);
	CHECK(meta.snapshotId == 19);
	CHECK(meta.builtStartedMs == 1200);
	CHECK(meta.builtCompletedMs == 1250);
	CHECK(meta.valid);
}

TEST_CASE("power snapshot build minute buckets aggregate current 1m 5m and 15m windows")
{
	PowerSnapshotBuildMinuteBucket buckets[kPowerSnapshotBuildMinuteBucketCount];
	resetPowerSnapshotBuildMinuteBuckets(buckets, kPowerSnapshotBuildMinuteBucketCount);

	recordPowerSnapshotBuildMinuteSample(buckets,
	                                     kPowerSnapshotBuildMinuteBucketCount,
	                                     10 * kPowerSnapshotBuildBucketMinuteMs + 1000,
	                                     120);
	recordPowerSnapshotBuildMinuteSample(buckets,
	                                     kPowerSnapshotBuildMinuteBucketCount,
	                                     10 * kPowerSnapshotBuildBucketMinuteMs + 5000,
	                                     180);
	recordPowerSnapshotBuildMinuteSample(buckets,
	                                     kPowerSnapshotBuildMinuteBucketCount,
	                                     8 * kPowerSnapshotBuildBucketMinuteMs + 1000,
	                                     300);
	recordPowerSnapshotBuildMinuteSample(buckets,
	                                     kPowerSnapshotBuildMinuteBucketCount,
	                                     0 * kPowerSnapshotBuildBucketMinuteMs + 1000,
	                                     700);

	const uint32_t nowMs = 10 * kPowerSnapshotBuildBucketMinuteMs + 59000;
	const PowerSnapshotBuildWindowStats oneMinute =
		aggregatePowerSnapshotBuildWindow(buckets, kPowerSnapshotBuildMinuteBucketCount, nowMs, 1);
	const PowerSnapshotBuildWindowStats fiveMinutes =
		aggregatePowerSnapshotBuildWindow(buckets, kPowerSnapshotBuildMinuteBucketCount, nowMs, 5);
	const PowerSnapshotBuildWindowStats fifteenMinutes =
		aggregatePowerSnapshotBuildWindow(buckets, kPowerSnapshotBuildMinuteBucketCount, nowMs, 15);

	CHECK(oneMinute.hasData);
	CHECK(oneMinute.minMs == 120);
	CHECK(oneMinute.maxMs == 180);
	CHECK(oneMinute.avgMs == 150);
	CHECK(oneMinute.count == 2);

	CHECK(fiveMinutes.hasData);
	CHECK(fiveMinutes.minMs == 120);
	CHECK(fiveMinutes.maxMs == 300);
	CHECK(fiveMinutes.avgMs == 200);
	CHECK(fiveMinutes.count == 3);

	CHECK(fifteenMinutes.hasData);
	CHECK(fifteenMinutes.minMs == 120);
	CHECK(fifteenMinutes.maxMs == 700);
	CHECK(fifteenMinutes.avgMs == 325);
	CHECK(fifteenMinutes.count == 4);
}

TEST_CASE("power snapshot build minute buckets overwrite stale slot data cleanly")
{
	PowerSnapshotBuildMinuteBucket buckets[kPowerSnapshotBuildMinuteBucketCount];
	resetPowerSnapshotBuildMinuteBuckets(buckets, kPowerSnapshotBuildMinuteBucketCount);

	recordPowerSnapshotBuildMinuteSample(buckets,
	                                     kPowerSnapshotBuildMinuteBucketCount,
	                                     1 * kPowerSnapshotBuildBucketMinuteMs + 1000,
	                                     140);
	recordPowerSnapshotBuildMinuteSample(buckets,
	                                     kPowerSnapshotBuildMinuteBucketCount,
	                                     16 * kPowerSnapshotBuildBucketMinuteMs + 1000,
	                                     220);

	const PowerSnapshotBuildWindowStats currentWindow =
		aggregatePowerSnapshotBuildWindow(buckets,
		                                  kPowerSnapshotBuildMinuteBucketCount,
		                                  16 * kPowerSnapshotBuildBucketMinuteMs + 2000,
		                                  15);

	CHECK(currentWindow.hasData);
	CHECK(currentWindow.minMs == 220);
	CHECK(currentWindow.maxMs == 220);
	CHECK(currentWindow.avgMs == 220);
	CHECK(currentWindow.count == 1);

	const PowerSnapshotBuildWindowStats staleWindow =
		aggregatePowerSnapshotBuildWindow(buckets,
		                                  kPowerSnapshotBuildMinuteBucketCount,
		                                  32 * kPowerSnapshotBuildBucketMinuteMs + 1000,
		                                  15);
	CHECK_FALSE(staleWindow.hasData);
	CHECK(staleWindow.count == 0);
}

TEST_CASE("power snapshot build minute buckets keep recent samples across millis rollover")
{
	PowerSnapshotBuildMinuteBucket buckets[kPowerSnapshotBuildMinuteBucketCount];
	resetPowerSnapshotBuildMinuteBuckets(buckets, kPowerSnapshotBuildMinuteBucketCount);

	recordPowerSnapshotBuildMinuteSample(buckets,
	                                     kPowerSnapshotBuildMinuteBucketCount,
	                                     UINT32_MAX - 1000,
	                                     220);
	recordPowerSnapshotBuildMinuteSample(buckets,
	                                     kPowerSnapshotBuildMinuteBucketCount,
	                                     5000,
	                                     140);
	recordPowerSnapshotBuildMinuteSample(buckets,
	                                     kPowerSnapshotBuildMinuteBucketCount,
	                                     1 * kPowerSnapshotBuildBucketMinuteMs + 5000,
	                                     300);

	const uint32_t nowMs = 1 * kPowerSnapshotBuildBucketMinuteMs + 59000;
	const PowerSnapshotBuildWindowStats oneMinute =
		aggregatePowerSnapshotBuildWindow(buckets, kPowerSnapshotBuildMinuteBucketCount, nowMs, 1);
	const PowerSnapshotBuildWindowStats fiveMinutes =
		aggregatePowerSnapshotBuildWindow(buckets, kPowerSnapshotBuildMinuteBucketCount, nowMs, 5);

	CHECK(oneMinute.hasData);
	CHECK(oneMinute.minMs == 300);
	CHECK(oneMinute.maxMs == 300);
	CHECK(oneMinute.avgMs == 300);
	CHECK(oneMinute.count == 1);

	CHECK(fiveMinutes.hasData);
	CHECK(fiveMinutes.minMs == 140);
	CHECK(fiveMinutes.maxMs == 300);
	CHECK(fiveMinutes.avgMs == 220);
	CHECK(fiveMinutes.count == 3);
}

TEST_CASE("power snapshot helpers coalesce dispatch requests only during snapshot build")
{
	CHECK(shouldQueueDispatchRequest(true, false, true));
	CHECK_FALSE(shouldQueueDispatchRequest(false, false, true));
	CHECK_FALSE(shouldQueueDispatchRequest(true, true, true));
	CHECK_FALSE(shouldQueueDispatchRequest(true, false, false));

	CHECK(shouldRejectDispatchRequest(true, false, false));
	CHECK(shouldRejectDispatchRequest(false, true, false));
	CHECK_FALSE(shouldRejectDispatchRequest(true, false, true));
	CHECK_FALSE(shouldRejectDispatchRequest(false, false, false));
}

TEST_CASE("power snapshot helpers classify dispatch block keys")
{
	CHECK(isDispatchBlockReadKey(REG_DISPATCH_RW_DISPATCH_START));
	CHECK(isDispatchBlockReadKey(REG_DISPATCH_RW_ACTIVE_POWER_1));
	CHECK(isDispatchBlockReadKey(REG_DISPATCH_RW_DISPATCH_MODE));
	CHECK(isDispatchBlockReadKey(REG_DISPATCH_RW_DISPATCH_SOC));
	CHECK(isDispatchBlockReadKey(REG_DISPATCH_RW_DISPATCH_TIME_1));
	CHECK_FALSE(isDispatchBlockReadKey(REG_GRID_METER_R_TOTAL_ACTIVE_POWER_1));
}

TEST_CASE("power snapshot helpers classify PV string block keys and expose stable block sizes")
{
	CHECK(kDispatchBlockStartReg == REG_DISPATCH_RW_DISPATCH_START);
	CHECK(kDispatchBlockRegisterCount == 9);
	CHECK(kPvStringBlockStartReg == REG_INVERTER_HOME_R_PV1_VOLTAGE);
	CHECK(kPvStringBlockRegisterCount == 24);
	CHECK(kPvStringCount == 6);

	CHECK(isPvStringBlockReadKey(REG_INVERTER_HOME_R_PV1_VOLTAGE));
	CHECK(isPvStringBlockReadKey(REG_INVERTER_HOME_R_PV3_CURRENT));
	CHECK(isPvStringBlockReadKey(REG_INVERTER_HOME_R_PV6_POWER_1));
	CHECK_FALSE(isPvStringBlockReadKey(REG_PV_METER_R_TOTAL_ACTIVE_POWER_1));
	CHECK_FALSE(isPvStringBlockReadKey(REG_INVERTER_HOME_R_INVERTER_TEMP));
}

TEST_CASE("power snapshot helpers classify coherent power entities explicitly")
{
	CHECK(isEssPowerSnapshotEntityId(mqttEntityId::entityBatPwr));
	CHECK(isEssPowerSnapshotEntityId(mqttEntityId::entityGridPwr));
	CHECK(isEssPowerSnapshotEntityId(mqttEntityId::entityPvPwr));
	CHECK(isEssPowerSnapshotEntityId(mqttEntityId::entityLoadPwr));
	CHECK_FALSE(isEssPowerSnapshotEntityId(mqttEntityId::entityBatSoc));
	CHECK_FALSE(isEssPowerSnapshotEntityId(mqttEntityId::entityInverterMode));
	CHECK_FALSE(isEssPowerSnapshotEntityId(mqttEntityId::entityDispatchStart));
}

TEST_CASE("power snapshot helpers expose scaled dispatch and PV values")
{
	CHECK(dispatchSocPercentFromRaw(95) == doctest::Approx(38.0f));
	CHECK(pvVoltageCurrentFromRaw(123) == doctest::Approx(12.3f));
}
