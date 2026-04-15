#include <doctest/doctest.h>

#include <cstring>

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

TEST_CASE("power snapshot diagnostics quantize elapsed millis in q10 units")
{
	CHECK(quantizeMillisToQ10(0) == 0);
	CHECK(quantizeMillisToQ10(4) == 0);
	CHECK(quantizeMillisToQ10(5) == 1);
	CHECK(quantizeMillisToQ10(14) == 1);
	CHECK(quantizeMillisToQ10(15) == 2);
	CHECK(quantizeMillisToQ10(UINT32_MAX) == UINT16_MAX);
}

TEST_CASE("power snapshot diagnostics capture transaction timing and result labels")
{
	PowerSnapshotDiagSubreadRuntime subread{};
	capturePowerSnapshotSubreadRuntime(subread,
	                                   437,
	                                   18,
	                                   9,
	                                   3,
	                                   1,
	                                   modbusRequestAndResponseStatusValues::readDataRegisterSuccess);

	CHECK(subread.totalQ10 == 44);
	CHECK(subread.waitQ10 == 18);
	CHECK(subread.quietQ10 == 9);
	CHECK(subread.attempts == 3);
	CHECK(subread.retries == 1);
	CHECK(subread.ok);
	CHECK(subread.result == modbusRequestAndResponseStatusValues::readDataRegisterSuccess);
	REQUIRE(subread.resultLabel != nullptr);
	CHECK(std::strcmp(subread.resultLabel, MODBUS_REQUEST_AND_RESPONSE_READ_DATA_REGISTER_SUCCESS_MQTT_DESC) == 0);
}

TEST_CASE("power snapshot diagnostics classify interesting events from reason masks")
{
	PowerSnapshotDiagSubreadRuntime subreads[kPowerSnapshotDiagSubreadCount]{};
	capturePowerSnapshotSubreadRuntime(subreads[static_cast<size_t>(PowerSnapshotDiagSubreadId::Battery)],
	                                   120,
	                                   4,
	                                   2,
	                                   1,
	                                   0,
	                                   modbusRequestAndResponseStatusValues::readDataRegisterSuccess);
	capturePowerSnapshotSubreadRuntime(subreads[static_cast<size_t>(PowerSnapshotDiagSubreadId::Grid)],
	                                   230,
	                                   10,
	                                   3,
	                                   2,
	                                   1,
	                                   modbusRequestAndResponseStatusValues::readDataRegisterSuccess);
	capturePowerSnapshotSubreadRuntime(subreads[static_cast<size_t>(PowerSnapshotDiagSubreadId::PvMeter)],
	                                   90,
	                                   5,
	                                   1,
	                                   1,
	                                   0,
	                                   modbusRequestAndResponseStatusValues::invalidFrame);
	capturePowerSnapshotSubreadRuntime(subreads[static_cast<size_t>(PowerSnapshotDiagSubreadId::PvBlock)],
	                                   140,
	                                   8,
	                                   2,
	                                   1,
	                                   0,
	                                   modbusRequestAndResponseStatusValues::readDataRegisterSuccess);

	const uint8_t reasonMask =
		computePowerSnapshotDiagReasonMask(subreads, kPowerSnapshotDiagSubreadCount, 51, 50);

	CHECK((reasonMask & PowerSnapshotDiagReasonSlowTotal) != 0);
	CHECK((reasonMask & PowerSnapshotDiagReasonRetry) != 0);
	CHECK((reasonMask & PowerSnapshotDiagReasonFailure) != 0);
	CHECK((reasonMask & PowerSnapshotDiagReasonLowLoad) != 0);
}

TEST_CASE("power snapshot diagnostics accumulate counters per subread")
{
	PowerSnapshotDiagCountsRuntime counts{};
	PowerSnapshotDiagSubreadRuntime subreads[kPowerSnapshotDiagSubreadCount]{};
	capturePowerSnapshotSubreadRuntime(subreads[static_cast<size_t>(PowerSnapshotDiagSubreadId::Battery)],
	                                   240,
	                                   12,
	                                   4,
	                                   2,
	                                   1,
	                                   modbusRequestAndResponseStatusValues::readDataRegisterSuccess);
	capturePowerSnapshotSubreadRuntime(subreads[static_cast<size_t>(PowerSnapshotDiagSubreadId::Grid)],
	                                   80,
	                                   6,
	                                   2,
	                                   1,
	                                   0,
	                                   modbusRequestAndResponseStatusValues::noResponse);
	capturePowerSnapshotSubreadRuntime(subreads[static_cast<size_t>(PowerSnapshotDiagSubreadId::PvMeter)],
	                                   205,
	                                   8,
	                                   3,
	                                   1,
	                                   0,
	                                   modbusRequestAndResponseStatusValues::invalidFrame);
	capturePowerSnapshotSubreadRuntime(subreads[static_cast<size_t>(PowerSnapshotDiagSubreadId::PvBlock)],
	                                   140,
	                                   7,
	                                   2,
	                                   1,
	                                   0,
	                                   modbusRequestAndResponseStatusValues::readDataRegisterSuccess);

	recordPowerSnapshotDiagCounts(counts,
	                              subreads,
	                              kPowerSnapshotDiagSubreadCount,
	                              PowerSnapshotDiagReasonRetry | PowerSnapshotDiagReasonFailure |
		                              PowerSnapshotDiagReasonLowLoad);

	CHECK(counts.interestingEventCount == 1);
	CHECK(counts.loadLowEventCount == 1);

	const auto &battery = counts.subreads[static_cast<size_t>(PowerSnapshotDiagSubreadId::Battery)];
	CHECK(battery.slowCount == 1);
	CHECK(battery.retryCount == 1);
	CHECK(battery.timeoutCount == 0);
	CHECK(battery.invalidFrameCount == 0);
	CHECK(battery.maxTotalQ10 == 24);

	const auto &grid = counts.subreads[static_cast<size_t>(PowerSnapshotDiagSubreadId::Grid)];
	CHECK(grid.slowCount == 0);
	CHECK(grid.retryCount == 0);
	CHECK(grid.timeoutCount == 1);
	CHECK(grid.invalidFrameCount == 0);
	CHECK(grid.maxTotalQ10 == 8);

	const auto &pvMeter = counts.subreads[static_cast<size_t>(PowerSnapshotDiagSubreadId::PvMeter)];
	CHECK(pvMeter.slowCount == 1);
	CHECK(pvMeter.retryCount == 0);
	CHECK(pvMeter.timeoutCount == 0);
	CHECK(pvMeter.invalidFrameCount == 1);
	CHECK(pvMeter.maxTotalQ10 == 21);

	const auto &pvBlock = counts.subreads[static_cast<size_t>(PowerSnapshotDiagSubreadId::PvBlock)];
	CHECK(pvBlock.slowCount == 0);
	CHECK(pvBlock.retryCount == 0);
	CHECK(pvBlock.timeoutCount == 0);
	CHECK(pvBlock.invalidFrameCount == 0);
	CHECK(pvBlock.maxTotalQ10 == 14);
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
