// Purpose: Validate persisted polling bucket parsing and interval validation.
#include "doctest/doctest.h"

#include <string>

#include "ConfigCodec.h"
#include "PollingConfig.h"

TEST_CASE("poll interval clamp enforces bounds")
{
	CHECK(clampPollInterval(0) == kPollIntervalMinSeconds);
	CHECK(clampPollInterval(kPollIntervalMinSeconds) == kPollIntervalMinSeconds);
	CHECK(clampPollInterval(kPollIntervalMaxSeconds) == kPollIntervalMaxSeconds);
	CHECK(clampPollInterval(kPollIntervalMaxSeconds + 1) == kPollIntervalMaxSeconds);
}

TEST_CASE("bucket map applies assignments and tracks invalid entries")
{
	mqttState entities[3]{};
	entities[0] = { mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, false, false, homeAssistantClass::haClassSelect };
	entities[1] = { mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqOneMin, false, false, homeAssistantClass::haClassNumber };
	entities[2] = { mqttEntityId::entityChargePwr, "Charge_Power", mqttUpdateFreq::freqFiveMin, false, false, homeAssistantClass::haClassNumber };

	MqttEntityRuntime rt[3]{};
	for (size_t i = 0; i < 3; ++i) {
		applyBucketToRuntime(rt[i], bucketIdFromFreq(entities[i].updateFreq));
	}

	uint32_t unknown = 0;
	uint32_t invalid = 0;
	uint32_t dup = 0;

	const char *map = "Op_Mode=one_min;SOC_Target=disabled;Missing=ten_sec;Charge_Power=bad;";
	CHECK(applyBucketMapString(map, entities, 3, rt, unknown, invalid, dup));
	CHECK(unknown == 1);
	CHECK(invalid == 1);
	CHECK(dup == 0);

	CHECK(rt[0].bucketId == BucketId::OneMin);
	CHECK(rt[1].bucketId == BucketId::Disabled);
	// Invalid bucket keeps previous assignment.
	CHECK(rt[2].bucketId == BucketId::FiveMin);
}

TEST_CASE("bucket map duplicate entries count and last write wins")
{
	mqttState entities[1]{};
	entities[0] = { mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, false, false, homeAssistantClass::haClassSelect };

	MqttEntityRuntime rt[1]{};
	applyBucketToRuntime(rt[0], bucketIdFromFreq(entities[0].updateFreq));

	uint32_t unknown = 0;
	uint32_t invalid = 0;
	uint32_t dup = 0;

	const char *map = "Op_Mode=one_min;Op_Mode=five_min;";
	CHECK(applyBucketMapString(map, entities, 1, rt, unknown, invalid, dup));
	CHECK(dup == 1);
	CHECK(rt[0].bucketId == BucketId::FiveMin);
}

TEST_CASE("bucket map syntax failure does not partially mutate runtime")
{
	mqttState entities[2]{};
	entities[0] = { mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, false, false, homeAssistantClass::haClassSelect };
	entities[1] = { mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqOneMin, false, false, homeAssistantClass::haClassNumber };

	MqttEntityRuntime rt[2]{};
	for (size_t i = 0; i < 2; ++i) {
		applyBucketToRuntime(rt[i], bucketIdFromFreq(entities[i].updateFreq));
	}

	uint32_t unknown = 0;
	uint32_t invalid = 0;
	uint32_t dup = 0;

	CHECK(!applyBucketMapString("Op_Mode=one_min;SOC_Target", entities, 2, rt, unknown, invalid, dup));
	CHECK(rt[0].bucketId == BucketId::TenSec);
	CHECK(rt[1].bucketId == BucketId::OneMin);
}

TEST_CASE("bucket map rejects entity counts above kMqttEntityMaxCount")
{
	mqttState entities[kMqttEntityMaxCount + 1]{};
	MqttEntityRuntime rt[kMqttEntityMaxCount + 1]{};
	for (size_t i = 0; i < kMqttEntityMaxCount + 1; ++i) {
		entities[i] = { mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, false, false, homeAssistantClass::haClassSelect };
		applyBucketToRuntime(rt[i], bucketIdFromFreq(entities[i].updateFreq));
	}

	uint32_t unknown = 0;
	uint32_t invalid = 0;
	uint32_t dup = 0;

	CHECK(!applyBucketMapString("Op_Mode=one_min;", entities, kMqttEntityMaxCount + 1, rt, unknown, invalid, dup));
}

TEST_CASE("legacy freq mapping uses bucket ids")
{
	CHECK(bucketIdFromLegacyFreq(static_cast<int>(mqttUpdateFreq::freqOneMin)) == BucketId::OneMin);
	CHECK(bucketIdFromLegacyFreq(9999) == BucketId::Unknown);
}

TEST_CASE("legacy values build stable bucket map")
{
	mqttState entities[2]{};
	entities[0] = { mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, false, false, homeAssistantClass::haClassSelect };
	entities[1] = { mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqOneMin, false, false, homeAssistantClass::haClassNumber };

	int legacy[2] = { static_cast<int>(mqttUpdateFreq::freqOneHour), static_cast<int>(mqttUpdateFreq::freqOneMin) };
	char out[128];
	size_t applied = 0;

	CHECK(buildBucketMapFromLegacy(entities, 2, legacy, out, sizeof(out), applied));
	CHECK(applied == 1);
	CHECK(std::string(out) == "Op_Mode=one_hour;");
}

TEST_CASE("legacy values skip invalid and defaults")
{
	mqttState entities[1]{};
	entities[0] = { mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, false, false, homeAssistantClass::haClassSelect };

	int legacy[1] = { 9999 };
	char out[64];
	size_t applied = 0;
	CHECK(!buildBucketMapFromLegacy(entities, 1, legacy, out, sizeof(out), applied));
	CHECK(applied == 0);
}

TEST_CASE("legacy bucket map builder fails when output buffer too small")
{
	mqttState entities[1]{};
	entities[0] = { mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, false, false, homeAssistantClass::haClassSelect };

	int legacy[1] = { static_cast<int>(mqttUpdateFreq::freqOneDay) };
	char out[8];
	size_t applied = 0;

	CHECK(!buildBucketMapFromLegacy(entities, 1, legacy, out, sizeof(out), applied));
	CHECK(applied == 0);
}

TEST_CASE("assignment builder emits stable overrides and round-trips")
{
	mqttState entities[2]{};
	entities[0] = { mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, false, false, homeAssistantClass::haClassSelect };
	entities[1] = { mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqOneMin, false, false, homeAssistantClass::haClassNumber };

	BucketId buckets[2] = { BucketId::TenSec, BucketId::User };
	char out[128];
	size_t applied = 0;

	CHECK(buildBucketMapFromAssignments(entities, 2, buckets, out, sizeof(out), applied));
	CHECK(applied == 1);
	CHECK(std::string(out) == "SOC_Target=user;");

	MqttEntityRuntime rt[2]{};
	for (size_t i = 0; i < 2; ++i) {
		applyBucketToRuntime(rt[i], bucketIdFromFreq(entities[i].updateFreq));
	}
	uint32_t unknown = 0, invalid = 0, dup = 0;
	CHECK(applyBucketMapString(out, entities, 2, rt, unknown, invalid, dup));
	CHECK(rt[1].bucketId == BucketId::User);
}
