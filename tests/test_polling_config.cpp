// Purpose: Validate persisted polling bucket parsing and interval validation.
#include "doctest/doctest.h"

#include <cstdlib>
#include <string>
#include <vector>

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

struct VisitCapture {
	std::string bucketMapValue;
	std::string pollIntervalValue;
	size_t visited = 0;
};

struct LegacyReaderCapture {
	int values[4]{};
	bool fail = false;
};

static bool
captureConfigEntry(const char *key, const char *value, void *context)
{
	auto &capture = *static_cast<VisitCapture *>(context);
	capture.visited++;
	if (std::string(key) == "bucket_map") {
		capture.bucketMapValue = value;
	} else if (std::string(key) == "poll_interval_s") {
		capture.pollIntervalValue = value;
	}
	return true;
}

static bool
captureLegacyValue(size_t index,
                   const mqttState * /* entity */,
                   int defaultValue,
                   int &storedValue,
                   void *context)
{
	auto &capture = *static_cast<LegacyReaderCapture *>(context);
	if (capture.fail) {
		return false;
	}
	storedValue = defaultValue;
	if (index < (sizeof(capture.values) / sizeof(capture.values[0]))) {
		storedValue = capture.values[index];
	}
	return true;
}

} // namespace

TEST_CASE("length-delimited copy accepts payloads beyond the legacy 512-byte callback cap")
{
	std::string input(900, 'x');
	char out[1024];
	char tooSmall[512];

	CHECK(copyLengthDelimitedString(input.c_str(), input.size(), out, sizeof(out)));
	CHECK(std::string(out) == input);
	CHECK_FALSE(copyLengthDelimitedString(input.c_str(), input.size(), tooSmall, sizeof(tooSmall)));
}

TEST_CASE("config entry visitor accepts large bucket_map values")
{
	std::string largeMap;
	for (int i = 0; i < 80; ++i) {
		largeMap += "Battery_Voltage=disabled;";
	}
	std::string payload = "{\"bucket_map\":\"" + largeMap + "\",\"poll_interval_s\":\"45\"}";
	char scratch[2048];
	VisitCapture capture{};

	CHECK(visitPollingConfigEntries(payload.c_str(), scratch, sizeof(scratch), captureConfigEntry, &capture));
	CHECK(capture.visited == 2);
	CHECK(capture.bucketMapValue == largeMap);
	CHECK(capture.pollIntervalValue == "45");
}

TEST_CASE("config entry validation rejects truncated payloads before apply")
{
	char scratch[256];
	CHECK(validatePollingConfigEntries("{\"poll_interval_s\":\"45\",\"bucket_map\":\"Op_Mode=one_min;\"}", scratch, sizeof(scratch)));
	CHECK_FALSE(validatePollingConfigEntries("{\"poll_interval_s\":\"45\",\"bucket_map\":\"Op_Mode=one_min;\"", scratch, sizeof(scratch)));
}

TEST_CASE("config entry visit aborts staged apply when bucket_map parsing fails")
{
	mqttState entities[1]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);

	struct Context {
		const mqttState *entities = nullptr;
		BucketId *stagedBuckets = nullptr;
		uint32_t *stagedInterval = nullptr;
		uint32_t unknown = 0;
		uint32_t invalid = 0;
		uint32_t dup = 0;
	};

	BucketId committedBuckets[1] = { BucketId::TenSec };
	BucketId stagedBuckets[1] = { BucketId::TenSec };
	uint32_t committedInterval = 30;
	uint32_t stagedInterval = committedInterval;
	char scratch[256];
	Context ctx{ entities, stagedBuckets, &stagedInterval };

	CHECK(validatePollingConfigEntries("{\"poll_interval_s\":\"45\",\"bucket_map\":\"Op_Mode\"}", scratch, sizeof(scratch)));
	const bool ok = visitPollingConfigEntries(
		"{\"poll_interval_s\":\"45\",\"bucket_map\":\"Op_Mode\"}",
		scratch,
		sizeof(scratch),
		[](const char *key, const char *value, void *context) -> bool {
			auto &ctx = *static_cast<Context *>(context);
			if (std::string(key) == "poll_interval_s") {
				*ctx.stagedInterval = clampPollInterval(static_cast<uint32_t>(std::strtoul(value, nullptr, 10)));
				return true;
			}
			if (std::string(key) == "bucket_map") {
				return applyBucketMapString(value,
				                            ctx.entities,
				                            1,
				                            ctx.stagedBuckets,
				                            ctx.unknown,
				                            ctx.invalid,
				                            ctx.dup);
			}
			return true;
		},
		&ctx);

	CHECK_FALSE(ok);
	CHECK(committedInterval == 30);
	CHECK(committedBuckets[0] == BucketId::TenSec);
	CHECK(stagedBuckets[0] == BucketId::TenSec);
}

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

TEST_CASE("bucket map detects deprecated descriptor-index tokens")
{
	CHECK(bucketMapUsesDescriptorIndices("#1=user;#0=five_min;"));
	CHECK(bucketMapUsesDescriptorIndices("Op_Mode=one_min;#0=five_min;"));
	CHECK_FALSE(bucketMapUsesDescriptorIndices("Op_Mode=one_min;SOC_Target=disabled;"));
	CHECK_FALSE(bucketMapUsesDescriptorIndices(""));
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
	CHECK(bucketIdFromLegacyFreq(5) == BucketId::Disabled);
	CHECK(bucketIdFromLegacyFreq(static_cast<int>(mqttUpdateFreq::freqNever)) == BucketId::Disabled);
	CHECK(bucketIdFromLegacyFreq(static_cast<int>(mqttUpdateFreq::freqDisabled)) == BucketId::Disabled);
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
	CHECK(std::string(out) == "Op_Mode=one_hour;");
}

TEST_CASE("legacy freqNever numeric value migrates to disabled instead of user")
{
	mqttState entities[1]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqOneMin, homeAssistantClass::haClassSelect);

	int legacy[1] = { 5 };
	char out[64];
	size_t applied = 0;

	CHECK(buildBucketMapFromLegacy(entities, 1, legacy, out, sizeof(out), applied));
	CHECK(applied == 1);
	CHECK(std::string(out) == "Op_Mode=disabled;");
}

TEST_CASE("legacy reader builds bucket map without staging the full value array")
{
	mqttState entities[2]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);
	entities[1] = makeEntity(mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqDisabled, homeAssistantClass::haClassNumber);

	LegacyReaderCapture capture{};
	capture.values[0] = static_cast<int>(mqttUpdateFreq::freqOneHour);
	capture.values[1] = static_cast<int>(mqttUpdateFreq::freqDisabled);

	char out[128];
	size_t applied = 0;

	CHECK(buildBucketMapFromLegacyReader(entities,
	                                     2,
	                                     captureLegacyValue,
	                                     &capture,
	                                     out,
	                                     sizeof(out),
	                                     applied));
	CHECK(applied == 1);
	CHECK(std::string(out) == "Op_Mode=one_hour;");
}

TEST_CASE("legacy reader surfaces read failures")
{
	mqttState entities[1]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);

	LegacyReaderCapture capture{};
	capture.fail = true;

	char out[32];
	size_t applied = 0;

	CHECK_FALSE(buildBucketMapFromLegacyReader(entities,
	                                           1,
	                                           captureLegacyValue,
	                                           &capture,
	                                           out,
	                                           sizeof(out),
	                                           applied));
}

TEST_CASE("assignment map persists stable entity names instead of descriptor indices")
{
	mqttState entities[2]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);
	entities[1] = makeEntity(mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqOneMin, homeAssistantClass::haClassNumber);

	BucketId buckets[2] = { BucketId::OneHour, BucketId::Disabled };
	char out[128];
	size_t applied = 0;

	CHECK(buildBucketMapFromAssignments(entities, 2, buckets, out, sizeof(out), applied));
	CHECK(applied == 2);
	CHECK(std::string(out) == "Op_Mode=one_hour;SOC_Target=disabled;");
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

TEST_CASE("assignment builder fails when the persisted override map exceeds the firmware limit")
{
	std::vector<mqttState> entities(90);
	std::vector<BucketId> buckets(90, BucketId::User);
	std::vector<std::string> names;
	names.reserve(entities.size());
	for (size_t i = 0; i < entities.size(); ++i) {
		char name[24];
		snprintf(name, sizeof(name), "Entity_%02u_Long_Name", static_cast<unsigned>(i));
		names.emplace_back(name);
		entities[i] = makeEntity(mqttEntityId::entityOpMode, names.back().c_str(), mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);
	}

	char out[2048];
	size_t applied = 0;
	CHECK_FALSE(buildBucketMapFromAssignments(entities.data(),
	                                          entities.size(),
	                                          buckets.data(),
	                                          out,
	                                          sizeof(out),
	                                          applied));
	CHECK(applied > 0);
}

TEST_CASE("assignment builder emits stable name overrides and round-trips")
{
	mqttState entities[2]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);
	entities[1] = makeEntity(mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqOneMin, homeAssistantClass::haClassNumber);

	BucketId buckets[2] = { BucketId::TenSec, BucketId::User };
	char out[128];
	size_t applied = 0;

	CHECK(buildBucketMapFromAssignments(entities, 2, buckets, out, sizeof(out), applied));
	CHECK(applied == 1);
	CHECK(std::string(out) == "SOC_Target=user;");

	BucketId roundTrip[2] = { BucketId::TenSec, BucketId::OneMin };
	uint32_t unknown = 0;
	uint32_t invalid = 0;
	uint32_t dup = 0;
	CHECK(applyBucketMapString(out, entities, 2, roundTrip, unknown, invalid, dup));
	CHECK(roundTrip[1] == BucketId::User);
}

TEST_CASE("active assignment chunk builder includes defaults and resumes from the next descriptor")
{
	mqttState entities[3]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);
	entities[1] = makeEntity(mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqOneMin, homeAssistantClass::haClassNumber);
	entities[2] = makeEntity(mqttEntityId::entityChargePwr, "Charge_Power", mqttUpdateFreq::freqFiveMin, homeAssistantClass::haClassNumber);

	BucketId buckets[3] = { BucketId::TenSec, BucketId::OneMin, BucketId::User };
	char chunk[40];
	size_t nextIndex = 0;
	size_t applied = 0;

	CHECK(buildActiveBucketMapChunkFromAssignments(entities, 3, buckets, 0, chunk, sizeof(chunk), nextIndex, applied));
	CHECK(applied == 2);
	CHECK(nextIndex == 2);
	CHECK(std::string(chunk) == "Op_Mode=ten_sec;SOC_Target=one_min;");

	CHECK(buildActiveBucketMapChunkFromAssignments(entities, 3, buckets, nextIndex, chunk, sizeof(chunk), nextIndex, applied));
	CHECK(applied == 1);
	CHECK(nextIndex == 3);
	CHECK(std::string(chunk) == "Charge_Power=user;");
}

TEST_CASE("active assignment chunk builder round-trips the full live schedule")
{
	mqttState entities[3]{};
	entities[0] = makeEntity(mqttEntityId::entityOpMode, "Op_Mode", mqttUpdateFreq::freqTenSec, homeAssistantClass::haClassSelect);
	entities[1] = makeEntity(mqttEntityId::entitySocTarget, "SOC_Target", mqttUpdateFreq::freqOneMin, homeAssistantClass::haClassNumber);
	entities[2] = makeEntity(mqttEntityId::entityChargePwr, "Charge_Power", mqttUpdateFreq::freqFiveMin, homeAssistantClass::haClassNumber);

	BucketId buckets[3] = { BucketId::TenSec, BucketId::Disabled, BucketId::User };
	BucketId roundTrip[3] = { BucketId::Disabled, BucketId::Disabled, BucketId::Disabled };
	uint32_t unknown = 0;
	uint32_t invalid = 0;
	uint32_t dup = 0;

	char chunk[32];
	size_t nextIndex = 0;
	size_t applied = 0;
	std::string combined;
	while (nextIndex < 3) {
		CHECK(buildActiveBucketMapChunkFromAssignments(entities, 3, buckets, nextIndex, chunk, sizeof(chunk), nextIndex, applied));
		if (applied == 0) {
			break;
		}
		combined += chunk;
	}

	CHECK(combined == "Op_Mode=ten_sec;Charge_Power=user;");
	CHECK(applyBucketMapString(combined.c_str(), entities, 3, roundTrip, unknown, invalid, dup));
	CHECK(roundTrip[0] == BucketId::TenSec);
	CHECK(roundTrip[1] == BucketId::Disabled);
	CHECK(roundTrip[2] == BucketId::User);
}
