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

