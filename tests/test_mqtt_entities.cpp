// Purpose: Verify catalog metadata stays flash-friendly and runtime state is
// derived from enabled entities rather than a full mutable per-entity array.

#include <set>
#include <cstring>
#include <string>
#include <vector>

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

TEST_CASE("mqtt entities: ids and mqtt names are unique and id lookup round-trips")
{
	const mqttState *desc = mqttEntitiesDesc();
	REQUIRE(desc != nullptr);
	const size_t count = mqttEntitiesCount();
	REQUIRE(count > 0);

	std::set<int> ids;
	std::set<std::string> names;

	for (size_t i = 0; i < count; ++i) {
		char name[64];
		mqttEntityNameCopy(&desc[i], name, sizeof(name));
		REQUIRE(name[0] != '\0');

		const int idValue = static_cast<int>(desc[i].entityId);
		CHECK(ids.insert(idValue).second);
		CHECK(names.insert(name).second);

		const mqttState *lookup = mqttEntityById(desc[i].entityId);
		REQUIRE(lookup != nullptr);
		CHECK(lookup == &desc[i]);
		CHECK(mqttEntityNameEquals(lookup, name));
	}
}

TEST_CASE("mqtt entities: copy and index helpers round-trip by id")
{
	const mqttState *desc = mqttEntitiesDesc();
	REQUIRE(desc != nullptr);
	const size_t count = mqttEntitiesCount();
	REQUIRE(count > 0);

	for (size_t i = 0; i < count; ++i) {
		mqttState byIndex{};
		mqttState byId{};
		size_t idx = count;
		REQUIRE(mqttEntityCopyByIndex(i, &byIndex));
		REQUIRE(mqttEntityCopyById(desc[i].entityId, &byId));
		REQUIRE(mqttEntityIndexById(desc[i].entityId, &idx));
		CHECK(idx == i);
		CHECK(byIndex.entityId == desc[i].entityId);
		CHECK(byId.entityId == desc[i].entityId);
		char name[64];
		mqttEntityNameCopy(&byIndex, name, sizeof(name));
		CHECK(mqttEntityNameEquals(&desc[i], name));
	}
}

TEST_CASE("mqtt entities: name lookup round-trips by index")
{
	const mqttState *desc = mqttEntitiesDesc();
	REQUIRE(desc != nullptr);
	const size_t count = mqttEntitiesCount();
	REQUIRE(count > 0);

	for (size_t i = 0; i < count; ++i) {
		char name[64];
		size_t idx = count;
		mqttEntityNameCopy(&desc[i], name, sizeof(name));
		REQUIRE(name[0] != '\0');
		REQUIRE(mqttEntityIndexByName(name, &idx));
		CHECK(idx == i);
	}

	size_t missingIdx = 0;
	CHECK_FALSE(mqttEntityIndexByName("Definitely_Not_A_Real_Entity", &missingIdx));
}

TEST_CASE("mqtt entities: full catalog copy mirrors descriptor metadata")
{
	const mqttState *desc = mqttEntitiesDesc();
	REQUIRE(desc != nullptr);
	const size_t count = mqttEntitiesCount();
	REQUIRE(count > 0);

	std::vector<mqttState> copied(count);
	REQUIRE(mqttEntityCopyCatalog(copied.data(), copied.size()));

	for (size_t i = 0; i < count; ++i) {
		CHECK(copied[i].entityId == desc[i].entityId);
		CHECK(copied[i].readKey == desc[i].readKey);
		CHECK(copied[i].family == desc[i].family);
		CHECK(copied[i].scope == desc[i].scope);
		char name[64];
		mqttEntityNameCopy(&copied[i], name, sizeof(name));
		CHECK(mqttEntityNameEquals(&desc[i], name));
	}
}

TEST_CASE("mqtt entities: legacy identity ids keep their pre-catalog ordering")
{
	CHECK(static_cast<uint16_t>(mqttEntityId::entityInverterSn)
	      < static_cast<uint16_t>(mqttEntityId::entityInverterVersion));
	CHECK(static_cast<uint16_t>(mqttEntityId::entityEmsSn)
	      < static_cast<uint16_t>(mqttEntityId::entityEmsVersion));
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

TEST_CASE("mqtt entities: legacy-compatible direct readbacks are exposed as register entities")
{
	const mqttState *batteryCurrent = mqttEntityById(mqttEntityId::entityBatteryCurrent);
	REQUIRE(batteryCurrent != nullptr);
	CHECK(mqttEntityNameEquals(batteryCurrent, "Battery_Current"));
	CHECK(batteryCurrent->family == MqttEntityFamily::Battery);
	CHECK(batteryCurrent->scope == MqttEntityScope::Inverter);
	CHECK(batteryCurrent->readKind == MqttEntityReadKind::Register);
	CHECK(batteryCurrent->updateFreq == mqttUpdateFreq::freqDisabled);
	CHECK(batteryCurrent->readKey == REG_BATTERY_HOME_R_CURRENT);

	const mqttState *dispatchStart = mqttEntityById(mqttEntityId::entityDispatchStart);
	REQUIRE(dispatchStart != nullptr);
	CHECK(mqttEntityNameEquals(dispatchStart, "Dispatch_Start"));
	CHECK(dispatchStart->readKind == MqttEntityReadKind::Register);
	CHECK(dispatchStart->readKey == REG_DISPATCH_RW_DISPATCH_START);

	const mqttState *dispatchMode = mqttEntityById(mqttEntityId::entityDispatchMode);
	REQUIRE(dispatchMode != nullptr);
	CHECK(mqttEntityNameEquals(dispatchMode, "Dispatch_Mode"));
	CHECK(dispatchMode->readKind == MqttEntityReadKind::Register);
	CHECK(dispatchMode->readKey == REG_DISPATCH_RW_DISPATCH_MODE);

	const mqttState *dispatchPower = mqttEntityById(mqttEntityId::entityDispatchPower);
	REQUIRE(dispatchPower != nullptr);
	CHECK(mqttEntityNameEquals(dispatchPower, "Dispatch_Power"));
	CHECK(dispatchPower->haClass == homeAssistantClass::haClassPower);
	CHECK(dispatchPower->readKey == REG_DISPATCH_RW_ACTIVE_POWER_1);

	const mqttState *dispatchSoc = mqttEntityById(mqttEntityId::entityDispatchSoc);
	REQUIRE(dispatchSoc != nullptr);
	CHECK(mqttEntityNameEquals(dispatchSoc, "Dispatch_SOC"));
	CHECK(dispatchSoc->haClass == homeAssistantClass::haClassBattery);
	CHECK(dispatchSoc->readKey == REG_DISPATCH_RW_DISPATCH_SOC);

	const mqttState *dispatchTime = mqttEntityById(mqttEntityId::entityDispatchTime);
	REQUIRE(dispatchTime != nullptr);
	CHECK(mqttEntityNameEquals(dispatchTime, "Dispatch_Time"));
	CHECK(dispatchTime->haClass == homeAssistantClass::haClassDuration);
	CHECK(dispatchTime->readKey == REG_DISPATCH_RW_DISPATCH_TIME_1);

	const mqttState *dispatchRequestStatus = mqttEntityById(mqttEntityId::entityDispatchRequestStatus);
	REQUIRE(dispatchRequestStatus != nullptr);
	CHECK(mqttEntityNameEquals(dispatchRequestStatus, "Dispatch_Request_Status"));
	CHECK(dispatchRequestStatus->haClass == homeAssistantClass::haClassInfo);
	CHECK(dispatchRequestStatus->readKind == MqttEntityReadKind::Derived);
	CHECK_FALSE(dispatchRequestStatus->subscribe);

	const mqttState *dispatchDuration = mqttEntityById(mqttEntityId::entityDispatchDuration);
	REQUIRE(dispatchDuration != nullptr);
	CHECK(mqttEntityNameEquals(dispatchDuration, "Dispatch_Duration"));
	CHECK(dispatchDuration->haClass == homeAssistantClass::haClassNumber);
	CHECK(dispatchDuration->readKind == MqttEntityReadKind::Control);
	CHECK(dispatchDuration->subscribe);
	CHECK_FALSE(mqttEntityIncludedInPublicSurface(dispatchDuration));

	const mqttState *dispatchRemaining = mqttEntityById(mqttEntityId::entityDispatchRemaining);
	REQUIRE(dispatchRemaining != nullptr);
	CHECK(mqttEntityNameEquals(dispatchRemaining, "Dispatch_Remaining"));
	CHECK(dispatchRemaining->haClass == homeAssistantClass::haClassDuration);
	CHECK(dispatchRemaining->readKind == MqttEntityReadKind::Derived);

	const mqttState *maxFeedin = mqttEntityById(mqttEntityId::entityMaxFeedinPercent);
	REQUIRE(maxFeedin != nullptr);
	CHECK(mqttEntityNameEquals(maxFeedin, "Max_Feedin_Percent"));
	CHECK(maxFeedin->haClass == homeAssistantClass::haClassNumber);
	CHECK(maxFeedin->readKey == REG_SYSTEM_CONFIG_RW_MAX_FEED_INTO_GRID_PERCENT);
	CHECK(maxFeedin->subscribe);
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

TEST_CASE("mqtt entities: retired dispatch controls stay out of public surfaces and config compaction")
{
	const size_t count = mqttEntitiesCount();
	REQUIRE(count > 0);

	std::vector<mqttState> sourceCatalog(count);
	std::vector<mqttState> compactedCatalog(count);
	std::vector<BucketId> sourceBuckets(count, BucketId::OneMin);
	std::vector<BucketId> compactedBuckets(count, BucketId::Disabled);
	REQUIRE(mqttEntityCopyCatalog(sourceCatalog.data(), sourceCatalog.size()));

	size_t retiredCount = 0;
	for (const mqttState &entity : sourceCatalog) {
		if (!mqttEntityIncludedInPublicSurface(&entity)) {
			retiredCount++;
		}
	}
	CHECK(retiredCount == 6);

	const std::vector<BucketId> sourceBucketsBefore = sourceBuckets;
	const size_t publicCount = mqttEntityCopyCompactedPublicSurfaceAssignments(
		sourceCatalog.data(),
		sourceBuckets.data(),
		count,
		compactedCatalog.data(),
		compactedBuckets.data());
	CHECK(publicCount == count - retiredCount);
	CHECK(sourceBuckets == sourceBucketsBefore);

	for (size_t i = 0; i < publicCount; ++i) {
		CHECK(mqttEntityIncludedInPublicSurface(&compactedCatalog[i]));
	}

	bool sawDispatchDuration = false;
	bool sawDispatchRequestStatus = false;
	for (size_t i = 0; i < publicCount; ++i) {
		sawDispatchDuration = sawDispatchDuration || mqttEntityNameEquals(&compactedCatalog[i], "Dispatch_Duration");
		sawDispatchRequestStatus =
			sawDispatchRequestStatus || mqttEntityNameEquals(&compactedCatalog[i], "Dispatch_Request_Status");
	}
	CHECK_FALSE(sawDispatchDuration);
	CHECK(sawDispatchRequestStatus);
}

TEST_CASE("mqtt entities: freqNever defaults stay discoverable without joining poll buckets")
{
	initMqttEntitiesRtIfNeeded(true);

	const mqttState *rs485Avail = mqttEntityById(mqttEntityId::entityRs485Avail);
	REQUIRE(rs485Avail != nullptr);

	const size_t idx = static_cast<size_t>(rs485Avail - mqttEntitiesDesc());
	REQUIRE(idx < kMqttEntityDescriptorCount);

	CHECK(mqttEntityBucketByIndex(idx) == BucketId::Disabled);
	CHECK(mqttEntityEffectiveFreqByIndex(idx) == mqttUpdateFreq::freqNever);
}

TEST_CASE("mqtt entities: Max_Feedin_Percent stays disabled until bucket-enabled")
{
	initMqttEntitiesRtIfNeeded(true);
	BucketId buckets[kMqttEntityDescriptorCount]{};
	REQUIRE(mqttEntityCopyBuckets(buckets, kMqttEntityDescriptorCount));

	const mqttState *maxFeedin = mqttEntityById(mqttEntityId::entityMaxFeedinPercent);
	REQUIRE(maxFeedin != nullptr);

	const size_t idx = static_cast<size_t>(maxFeedin - mqttEntitiesDesc());
	REQUIRE(idx < kMqttEntityDescriptorCount);

	CHECK(mqttEntityBucketByIndex(idx) == BucketId::Disabled);
	CHECK(mqttEntityEffectiveFreqByIndex(idx) == mqttUpdateFreq::freqDisabled);

	buckets[idx] = BucketId::OneMin;
	REQUIRE(mqttEntityApplyBuckets(buckets, kMqttEntityDescriptorCount));
	CHECK(mqttEntityEffectiveFreqByIndex(idx) == mqttUpdateFreq::freqOneMin);

	buckets[idx] = BucketId::Disabled;
	REQUIRE(mqttEntityApplyBuckets(buckets, kMqttEntityDescriptorCount));
	CHECK(mqttEntityEffectiveFreqByIndex(idx) == mqttUpdateFreq::freqDisabled);
}

TEST_CASE("mqtt entities: freqNever defaults can still be overridden into a real poll bucket")
{
	initMqttEntitiesRtIfNeeded(true);
	BucketId buckets[kMqttEntityDescriptorCount]{};
	REQUIRE(mqttEntityCopyBuckets(buckets, kMqttEntityDescriptorCount));

	const mqttState *gridAvail = mqttEntityById(mqttEntityId::entityGridAvail);
	REQUIRE(gridAvail != nullptr);

	const size_t idx = static_cast<size_t>(gridAvail - mqttEntitiesDesc());
	REQUIRE(idx < kMqttEntityDescriptorCount);

	buckets[idx] = BucketId::OneMin;
	REQUIRE(mqttEntityApplyBuckets(buckets, kMqttEntityDescriptorCount));
	CHECK(mqttEntityEffectiveFreqByIndex(idx) == mqttUpdateFreq::freqOneMin);

	buckets[idx] = BucketId::Disabled;
	REQUIRE(mqttEntityApplyBuckets(buckets, kMqttEntityDescriptorCount));
	CHECK(mqttEntityEffectiveFreqByIndex(idx) == mqttUpdateFreq::freqDisabled);
}

TEST_CASE("mqtt entities: controller diagnostics append after the legacy persisted entity set")
{
	const mqttState *desc = mqttEntitiesDesc();
	const size_t count = mqttEntitiesCount();
	size_t registerValueIdx = count;
	size_t batteryCurrentIdx = count;
	size_t maxFeedinIdx = count;
	size_t dispatchRequestStatusIdx = count;
	size_t rs485ErrorsIdx = count;
	size_t budgetExceededIdx = count;

	for (size_t i = 0; i < count; ++i) {
		if (mqttEntityNameEquals(&desc[i], "Register_Value")) {
			registerValueIdx = i;
		}
		if (mqttEntityNameEquals(&desc[i], "Battery_Current")) {
			batteryCurrentIdx = i;
		}
		if (mqttEntityNameEquals(&desc[i], "Max_Feedin_Percent")) {
			maxFeedinIdx = i;
		}
		if (mqttEntityNameEquals(&desc[i], "Dispatch_Request_Status")) {
			dispatchRequestStatusIdx = i;
		}
		if (mqttEntityNameEquals(&desc[i], "A2M_RS485_Errors")) {
			rs485ErrorsIdx = i;
		}
		if (mqttEntityNameEquals(&desc[i], "Polling_Budget_Exceeded")) {
			budgetExceededIdx = i;
		}
	}

	REQUIRE(registerValueIdx < count);
	REQUIRE(batteryCurrentIdx < count);
	REQUIRE(maxFeedinIdx < count);
	REQUIRE(dispatchRequestStatusIdx < count);
	REQUIRE(rs485ErrorsIdx < count);
	REQUIRE(budgetExceededIdx < count);
	CHECK(registerValueIdx < batteryCurrentIdx);
	CHECK(batteryCurrentIdx < maxFeedinIdx);
	CHECK(maxFeedinIdx < dispatchRequestStatusIdx);
	CHECK(maxFeedinIdx < rs485ErrorsIdx);
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

TEST_CASE("mqtt entities: preview bucket apply does not mutate runtime state")
{
	initMqttEntitiesRtIfNeeded(true);

	BucketId current[kMqttEntityDescriptorCount]{};
	REQUIRE(mqttEntityCopyBuckets(current, kMqttEntityDescriptorCount));

	const mqttState *desc = mqttEntitiesDesc();
	size_t uptimeIdx = kMqttEntityDescriptorCount;
	for (size_t i = 0; i < kMqttEntityDescriptorCount; ++i) {
		if (mqttEntityNameEquals(&desc[i], "A2M_uptime")) {
			uptimeIdx = i;
			break;
		}
	}
	REQUIRE(uptimeIdx < kMqttEntityDescriptorCount);

	BucketId preview[kMqttEntityDescriptorCount]{};
	std::memcpy(preview, current, sizeof(preview));
	const BucketId original = current[uptimeIdx];
	const BucketId alternate = (original == BucketId::OneMin) ? BucketId::TenSec : BucketId::OneMin;
	preview[uptimeIdx] = alternate;

	REQUIRE(mqttEntityCanApplyBuckets(preview, kMqttEntityDescriptorCount));
	CHECK(mqttEntityBucketByIndex(uptimeIdx) == original);

	BucketId after[kMqttEntityDescriptorCount]{};
	REQUIRE(mqttEntityCopyBuckets(after, kMqttEntityDescriptorCount));
	CHECK(after[uptimeIdx] == original);
}

TEST_CASE("mqtt entities: State_of_Charge can move from one_min to ten_sec")
{
	initMqttEntitiesRtIfNeeded(true);

	BucketId current[kMqttEntityDescriptorCount]{};
	REQUIRE(mqttEntityCopyBuckets(current, kMqttEntityDescriptorCount));

	const mqttState *desc = mqttEntitiesDesc();
	size_t socIdx = kMqttEntityDescriptorCount;
	for (size_t i = 0; i < kMqttEntityDescriptorCount; ++i) {
		if (mqttEntityNameEquals(&desc[i], "State_of_Charge")) {
			socIdx = i;
			break;
		}
	}
	REQUIRE(socIdx < kMqttEntityDescriptorCount);
	REQUIRE(current[socIdx] == BucketId::OneMin);

	BucketId preview[kMqttEntityDescriptorCount]{};
	std::memcpy(preview, current, sizeof(preview));
	preview[socIdx] = BucketId::TenSec;

	REQUIRE(mqttEntityCanApplyBuckets(preview, kMqttEntityDescriptorCount));
}
