// Purpose: Validate persisted polling bucket parsing and interval validation.
#include "doctest/doctest.h"

#include <string>

#include "ConfigCodec.h"
#include "PollingConfig.h"

namespace {

static mqttState
makeEntity(mqttEntityId id,
           const char *name,
           mqttUpdateFreq freq,
           homeAssistantClass haClass)
{
	return { name,
	         0,
	         id,
	         freq,
	         haClass,
	         MqttEntityFamily::System,
	         MqttEntityScope::Inverter,
	         MqttEntityReadKind::Control,
	         false,
	         false,
	         false };
}

} // namespace

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
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);
	entities[1] = makeEntity(mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqOneMin, homeAssistantClass::haClassNumber);
	entities[2] = makeEntity(mqttEntityId::entityChargePwr, "Charge_Power", mqttUpdateFreq::freqFiveMin, homeAssistantClass::haClassNumber);

	BucketId buckets[3] = { BucketId::TenSec, BucketId::OneMin, BucketId::FiveMin };
	uint32_t unknown = 0;
	uint32_t invalid = 0;
	uint32_t dup = 0;

	const char *map = "Op_Mode=one_min;SOC_Target=disabled;Missing=ten_sec;Charge_Power=bad;";
	CHECK(applyBucketMapString(map, entities, 3, buckets, unknown, invalid, dup));
	CHECK(unknown == 1);
	CHECK(invalid == 1);
	CHECK(dup == 0);

	CHECK(buckets[0] == BucketId::OneMin);
	CHECK(buckets[1] == BucketId::Disabled);
	CHECK(buckets[2] == BucketId::FiveMin);
}

TEST_CASE("bucket map supports compact descriptor indices")
{
	mqttState entities[2]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);
	entities[1] = makeEntity(mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqOneMin, homeAssistantClass::haClassNumber);

	BucketId buckets[2] = { BucketId::TenSec, BucketId::OneMin };
	uint32_t unknown = 0;
	uint32_t invalid = 0;
	uint32_t dup = 0;

	CHECK(applyBucketMapString("#1=user;#0=five_min;", entities, 2, buckets, unknown, invalid, dup));
	CHECK(unknown == 0);
	CHECK(invalid == 0);
	CHECK(dup == 0);
	CHECK(buckets[0] == BucketId::FiveMin);
	CHECK(buckets[1] == BucketId::User);
}

TEST_CASE("bucket map duplicate entries count and last write wins")
{
	mqttState entities[1]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);

	BucketId buckets[1] = { BucketId::TenSec };
	uint32_t unknown = 0;
	uint32_t invalid = 0;
	uint32_t dup = 0;

	const char *map = "Op_Mode=one_min;#0=five_min;";
	CHECK(applyBucketMapString(map, entities, 1, buckets, unknown, invalid, dup));
	CHECK(dup == 1);
	CHECK(buckets[0] == BucketId::FiveMin);
}

TEST_CASE("bucket map syntax failure does not partially mutate assignments")
{
	mqttState entities[2]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);
	entities[1] = makeEntity(mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqOneMin, homeAssistantClass::haClassNumber);

	BucketId buckets[2] = { BucketId::TenSec, BucketId::OneMin };
	uint32_t unknown = 0;
	uint32_t invalid = 0;
	uint32_t dup = 0;

	CHECK(!applyBucketMapString("Op_Mode=one_min;SOC_Target", entities, 2, buckets, unknown, invalid, dup));
	CHECK(buckets[0] == BucketId::TenSec);
	CHECK(buckets[1] == BucketId::OneMin);
}

TEST_CASE("bucket map rejects entity counts above compiled descriptor count")
{
	mqttState entities[kMqttEntityDescriptorCount + 1]{};
	BucketId buckets[kMqttEntityDescriptorCount + 1]{};
	for (size_t i = 0; i < kMqttEntityDescriptorCount + 1; ++i) {
		entities[i] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);
		buckets[i] = BucketId::TenSec;
	}

	uint32_t unknown = 0;
	uint32_t invalid = 0;
	uint32_t dup = 0;

	CHECK(!applyBucketMapString("Op_Mode=one_min;", entities, kMqttEntityDescriptorCount + 1, buckets, unknown, invalid, dup));
}

TEST_CASE("legacy freq mapping uses bucket ids")
{
	CHECK(bucketIdFromLegacyFreq(static_cast<int>(mqttUpdateFreq::freqOneMin)) == BucketId::OneMin);
	CHECK(bucketIdFromLegacyFreq(9999) == BucketId::Unknown);
}

TEST_CASE("legacy values build compact stable bucket map")
{
	mqttState entities[2]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);
	entities[1] = makeEntity(mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqOneMin, homeAssistantClass::haClassNumber);

	int legacy[2] = { static_cast<int>(mqttUpdateFreq::freqOneHour), static_cast<int>(mqttUpdateFreq::freqOneMin) };
	char out[128];
	size_t applied = 0;

	CHECK(buildBucketMapFromLegacy(entities, 2, legacy, out, sizeof(out), applied));
	CHECK(applied == 1);
	CHECK(std::string(out) == "#0=one_hour;");
}

TEST_CASE("legacy values skip invalid and defaults")
{
	mqttState entities[1]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);

	int legacy[1] = { 9999 };
	char out[64];
	size_t applied = 0;
	CHECK(!buildBucketMapFromLegacy(entities, 1, legacy, out, sizeof(out), applied));
	CHECK(applied == 0);
}

TEST_CASE("legacy bucket map builder fails when output buffer too small")
{
	mqttState entities[1]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);

	int legacy[1] = { static_cast<int>(mqttUpdateFreq::freqOneDay) };
	char out[8];
	size_t applied = 0;

	CHECK(!buildBucketMapFromLegacy(entities, 1, legacy, out, sizeof(out), applied));
	CHECK(applied == 0);
}

TEST_CASE("assignment builder emits compact overrides and round-trips")
{
	mqttState entities[2]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);
	entities[1] = makeEntity(mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqOneMin, homeAssistantClass::haClassNumber);

	BucketId buckets[2] = { BucketId::TenSec, BucketId::User };
	char out[128];
	size_t applied = 0;

	CHECK(buildBucketMapFromAssignments(entities, 2, buckets, out, sizeof(out), applied));
	CHECK(applied == 1);
	CHECK(std::string(out) == "#1=user;");

	BucketId roundTrip[2] = { BucketId::TenSec, BucketId::OneMin };
	uint32_t unknown = 0;
	uint32_t invalid = 0;
	uint32_t dup = 0;
	CHECK(applyBucketMapString(out, entities, 2, roundTrip, unknown, invalid, dup));
	CHECK(roundTrip[1] == BucketId::User);
}
