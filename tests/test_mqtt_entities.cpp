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
