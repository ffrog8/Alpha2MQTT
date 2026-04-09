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
