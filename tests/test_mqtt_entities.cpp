// Purpose: Verify catalog metadata stays flash-friendly and runtime state is
// derived from enabled entities rather than a full mutable per-entity array.

#include <cstring>

#include <doctest/doctest.h>

#include "BucketScheduler.h"
#include "MqttEntities.h"

TEST_CASE("mqtt entities: descriptor table exists")
{
	CHECK(mqttEntitiesDesc() != nullptr);
	CHECK(mqttEntitiesCount() > 0);
	CHECK(mqttEntitiesCount() == kMqttEntityDescriptorCount);

	const mqttState *desc = mqttEntitiesDesc();
	const size_t count = mqttEntitiesCount();
	const size_t samples = (count < 3) ? count : 3;
	for (size_t i = 0; i < samples; ++i) {
		char name[64];
		mqttEntityNameCopy(&desc[i], name, sizeof(name));
		CHECK(std::strlen(name) > 0);
	}
}

TEST_CASE("mqtt entities: runtime initializes without allocating a full mutable descriptor array")
{
	const bool initialAvailable = mqttEntitiesRtAvailable();

	initMqttEntitiesRtIfNeeded(false);
	CHECK(mqttEntitiesRtAvailable() == initialAvailable);

	initMqttEntitiesRtIfNeeded(true);
	CHECK(mqttEntitiesRtAvailable());

	BucketId buckets[kMqttEntityDescriptorCount]{};
	REQUIRE(mqttEntityCopyBuckets(buckets, kMqttEntityDescriptorCount));

	const mqttState *desc = mqttEntitiesDesc();
	for (size_t i = 0; i < kMqttEntityDescriptorCount; ++i) {
		CHECK(buckets[i] == bucketIdFromFreq(desc[i].updateFreq));
	}

	const MqttEntityActivePlan *plan = mqttActivePlan();
	REQUIRE(plan != nullptr);

	size_t expectedActive = 0;
	for (size_t i = 0; i < kMqttEntityDescriptorCount; ++i) {
		if (buckets[i] != BucketId::Disabled) {
			expectedActive++;
		}
	}
	CHECK(plan->activeCount == expectedActive);
	CHECK(plan->tenSec.transactionCount <= plan->tenSec.count);
	CHECK(plan->oneMin.transactionCount <= plan->oneMin.count);
	CHECK(plan->fiveMin.transactionCount <= plan->fiveMin.count);
	CHECK(plan->oneHour.transactionCount <= plan->oneHour.count);
	CHECK(plan->oneDay.transactionCount <= plan->oneDay.count);
	CHECK(plan->user.transactionCount <= plan->user.count);
}

TEST_CASE("mqtt entities: ESS snapshot dependency metadata matches expected entities")
{
	initMqttEntitiesRtIfNeeded(true);

	const mqttState *desc = mqttEntitiesDesc();
	const size_t count = mqttEntitiesCount();

	auto findIndex = [&](const char *name) -> size_t {
		for (size_t i = 0; i < count; ++i) {
			if (mqttEntityNameEquals(&desc[i], name)) {
				return i;
			}
		}
		return count;
	};

	const size_t socIdx = findIndex("State_of_Charge");
	REQUIRE(socIdx < count);
	CHECK(mqttEntityNeedsEssSnapshotByIndex(socIdx));

	const size_t essIdx = findIndex("ESS_Power");
	REQUIRE(essIdx < count);
	CHECK(mqttEntityNeedsEssSnapshotByIndex(essIdx));

	const size_t uptimeIdx = findIndex("A2M_uptime");
	REQUIRE(uptimeIdx < count);
	CHECK_FALSE(mqttEntityNeedsEssSnapshotByIndex(uptimeIdx));
}

TEST_CASE("mqtt entities: expanded catalog exposes metadata for direct register entities")
{
	const mqttState *gridVoltage = mqttEntityById(mqttEntityId::entityGridVoltageA);
	REQUIRE(gridVoltage != nullptr);
	CHECK(mqttEntityNameEquals(gridVoltage, "Grid_Voltage_A"));
	CHECK(gridVoltage->family == MqttEntityFamily::Grid);
	CHECK(gridVoltage->scope == MqttEntityScope::Inverter);
	CHECK(gridVoltage->readKind == MqttEntityReadKind::Register);
	CHECK(gridVoltage->updateFreq == mqttUpdateFreq::freqDisabled);
	CHECK(gridVoltage->readKey == REG_GRID_METER_R_VOLTAGE_OF_A_PHASE);
}

TEST_CASE("mqtt entities: controller diagnostics include runtime polling signals")
{
	const mqttState *rs485Err = mqttEntityById(mqttEntityId::entityRs485Errors);
	REQUIRE(rs485Err != nullptr);
	CHECK(rs485Err->family == MqttEntityFamily::Controller);
	CHECK(rs485Err->scope == MqttEntityScope::Controller);
	CHECK(rs485Err->updateFreq == mqttUpdateFreq::freqOneMin);

	const mqttState *budgetExceeded = mqttEntityById(mqttEntityId::entityPollingBudgetExceeded);
	REQUIRE(budgetExceeded != nullptr);
	CHECK(mqttEntityNameEquals(budgetExceeded, "Polling_Budget_Exceeded"));
	CHECK(budgetExceeded->family == MqttEntityFamily::Controller);
	CHECK(budgetExceeded->scope == MqttEntityScope::Controller);
	CHECK(budgetExceeded->readKind == MqttEntityReadKind::Derived);

	const mqttState *budgetUsed1m = mqttEntityById(mqttEntityId::entityPollingBudgetUsedMs1m);
	REQUIRE(budgetUsed1m != nullptr);
	CHECK(budgetUsed1m->updateFreq == mqttUpdateFreq::freqDisabled);
	CHECK(budgetUsed1m->family == MqttEntityFamily::Controller);
}

TEST_CASE("mqtt entities: controller diagnostics append after the legacy persisted entity set")
{
	const mqttState *desc = mqttEntitiesDesc();
	const size_t count = mqttEntitiesCount();
	size_t registerValueIdx = count;
	size_t rs485ErrorsIdx = count;
	size_t budgetExceededIdx = count;

	for (size_t i = 0; i < count; ++i) {
		if (mqttEntityNameEquals(&desc[i], "Register_Value")) {
			registerValueIdx = i;
		}
		if (mqttEntityNameEquals(&desc[i], "A2M_RS485_Errors")) {
			rs485ErrorsIdx = i;
		}
		if (mqttEntityNameEquals(&desc[i], "Polling_Budget_Exceeded")) {
			budgetExceededIdx = i;
		}
	}

	REQUIRE(registerValueIdx < count);
	REQUIRE(rs485ErrorsIdx < count);
	REQUIRE(budgetExceededIdx < count);
	CHECK(registerValueIdx < rs485ErrorsIdx);
	CHECK(registerValueIdx < budgetExceededIdx);
}

TEST_CASE("mqtt entities: shared direct-register reads collapse into one poll transaction")
{
	initMqttEntitiesRtIfNeeded(true);
	BucketId buckets[kMqttEntityDescriptorCount]{};
	REQUIRE(mqttEntityCopyBuckets(buckets, kMqttEntityDescriptorCount));

	const mqttState *desc = mqttEntitiesDesc();
	size_t batTempIdx = kMqttEntityDescriptorCount;
	size_t maxCellTempIdx = kMqttEntityDescriptorCount;
	for (size_t i = 0; i < kMqttEntityDescriptorCount; ++i) {
		if (mqttEntityNameEquals(&desc[i], "Battery_Temp")) {
			batTempIdx = i;
		}
		if (mqttEntityNameEquals(&desc[i], "Max_Cell_Temperature")) {
			maxCellTempIdx = i;
		}
	}
	REQUIRE(batTempIdx < kMqttEntityDescriptorCount);
	REQUIRE(maxCellTempIdx < kMqttEntityDescriptorCount);

	buckets[batTempIdx] = BucketId::FiveMin;
	buckets[maxCellTempIdx] = BucketId::FiveMin;
	REQUIRE(mqttEntityApplyBuckets(buckets, kMqttEntityDescriptorCount));

	const MqttEntityActivePlan *plan = mqttActivePlan();
	REQUIRE(plan != nullptr);
	REQUIRE(plan->fiveMin.transactionCount > 0);
	CHECK(plan->fiveMin.transactionCount < plan->fiveMin.count);

	bool foundSharedRegisterGroup = false;
	for (size_t txnIdx = 0; txnIdx < plan->fiveMin.transactionCount; ++txnIdx) {
		const MqttPollTransaction &txn = plan->fiveMin.transactions[txnIdx];
		if (txn.kind == MqttPollTransactionKind::RegisterFanout &&
		    txn.readKey == REG_BATTERY_HOME_R_MAX_CELL_TEMPERATURE &&
		    txn.entityCount == 2) {
			foundSharedRegisterGroup = true;
			break;
		}
	}
	CHECK(foundSharedRegisterGroup);

	buckets[maxCellTempIdx] = BucketId::Disabled;
	REQUIRE(mqttEntityApplyBuckets(buckets, kMqttEntityDescriptorCount));
}

TEST_CASE("mqtt entities: bucket overrides are queryable before rebuilding the active plan")
{
	initMqttEntitiesRtIfNeeded(true);
	BucketId buckets[kMqttEntityDescriptorCount]{};
	REQUIRE(mqttEntityCopyBuckets(buckets, kMqttEntityDescriptorCount));

	const mqttState *desc = mqttEntitiesDesc();
	size_t uptimeIdx = kMqttEntityDescriptorCount;
	for (size_t i = 0; i < kMqttEntityDescriptorCount; ++i) {
		if (mqttEntityNameEquals(&desc[i], "A2M_uptime")) {
			uptimeIdx = i;
			break;
		}
	}
	REQUIRE(uptimeIdx < kMqttEntityDescriptorCount);

	buckets[uptimeIdx] = BucketId::OneMin;
	REQUIRE(mqttEntityApplyBuckets(buckets, kMqttEntityDescriptorCount));

	CHECK(mqttEntityBucketByIndex(uptimeIdx) == BucketId::OneMin);
	REQUIRE(mqttEntityCopyBuckets(buckets, kMqttEntityDescriptorCount));
	CHECK(buckets[uptimeIdx] == BucketId::OneMin);

	const MqttEntityActivePlan *plan = mqttActivePlan();
	REQUIRE(plan != nullptr);
	CHECK(plan->oneMin.count > 0);
}
