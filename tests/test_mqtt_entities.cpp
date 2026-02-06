// Purpose: Verify MQTT entity descriptor/runtime split stays flash-friendly and
// that runtime state is only allocated when explicitly enabled.
// Invariants: Runtime allocation is one-way per boot; tests do not attempt to free.

#include <doctest/doctest.h>

#include "MqttEntities.h"

#include <cstring>

TEST_CASE("mqtt entities: descriptor table exists")
{
	CHECK(mqttEntitiesDesc() != nullptr);
	CHECK(mqttEntitiesCount() > 0);

	const mqttState *desc = mqttEntitiesDesc();
	const size_t count = mqttEntitiesCount();
	const size_t samples = (count < 3) ? count : 3;
	for (size_t i = 0; i < samples; ++i) {
		CHECK(desc[i].mqttName != nullptr);
		CHECK(std::strlen(desc[i].mqttName) > 0);
	}
}

TEST_CASE("mqtt entities: runtime is allocated only when enabled")
{
	const bool initialAvailable = mqttEntitiesRtAvailable();

	initMqttEntitiesRtIfNeeded(false);
	CHECK(mqttEntitiesRtAvailable() == initialAvailable);

	initMqttEntitiesRtIfNeeded(true);
	CHECK(mqttEntitiesRtAvailable());
	REQUIRE(mqttEntitiesRt() != nullptr);

	const mqttState *desc = mqttEntitiesDesc();
	const MqttEntityRuntime *rt = mqttEntitiesRt();
	const size_t count = mqttEntitiesCount();
	const size_t samples = (count < 5) ? count : 5;
	for (size_t i = 0; i < samples; ++i) {
		CHECK(rt[i].defaultFreq == desc[i].updateFreq);
		CHECK(rt[i].effectiveFreq == desc[i].updateFreq);
	}
}

TEST_CASE("mqtt entities: ESS snapshot dependency metadata matches expected entities")
{
	initMqttEntitiesRtIfNeeded(true);
	REQUIRE(mqttEntitiesRtAvailable());

	const mqttState *desc = mqttEntitiesDesc();
	const size_t count = mqttEntitiesCount();

	auto findIndex = [&](const char *name) -> size_t {
		for (size_t i = 0; i < count; ++i) {
			if (std::strcmp(desc[i].mqttName, name) == 0) {
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
