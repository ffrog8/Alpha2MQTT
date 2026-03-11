// Purpose: Provide the flash-resident MQTT entity catalog and a sparse runtime
// selection model for enabled polling work.
// Responsibilities: Resolve per-entity bucket overrides, maintain the enabled
// active poll plan, and avoid per-catalog mutable allocations on ESP8266.
#include "../include/MqttEntities.h"

#include "../include/BucketScheduler.h"

#include <new>
#include <cstdio>
#include <cstring>

#if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
#include <pgmspace.h>
#endif

namespace {

#if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
#define MQTT_ENTITY_NAME_DECL(id, name) static const char id##_name[] PROGMEM = name;
#else
#define MQTT_ENTITY_NAME_DECL(id, name) static const char id##_name[] = name;
#endif

#define MQTT_ENTITY_ROW(id, name, ...) MQTT_ENTITY_NAME_DECL(id, name)
#include "../include/MqttEntityCatalogRows.h"
#undef MQTT_ENTITY_ROW
#undef MQTT_ENTITY_NAME_DECL

#define MQTT_ENTITY_ROW(id, name, freq, subscribe, retain, haClass, family, scope, readKind, readKey, needsEssSnapshot) \
	{ id##_name, static_cast<uint16_t>(readKey), id, freq, haClass, family, scope, readKind, subscribe, retain, needsEssSnapshot },

static const mqttState kMqttEntities[] = {
#include "../include/MqttEntityCatalogRows.h"
};

#undef MQTT_ENTITY_ROW

static_assert(sizeof(kMqttEntities) / sizeof(kMqttEntities[0]) == kMqttEntityDescriptorCount,
              "kMqttEntityDescriptorCount must match kMqttEntities length");

struct RuntimeState {
	bool initialized = false;
	bool planDirty = true;
	MqttEntityBucketOverride *overrides = nullptr;
	size_t overrideCount = 0;
	MqttEntityActivePlan plan{};
};

static RuntimeState g_runtime;

static void
resetBucket(MqttEntityActiveBucket &bucket)
{
	delete[] bucket.members;
	bucket.members = nullptr;
	bucket.count = 0;
	bucket.hasEssSnapshot = false;
}

static void
resetActivePlan(MqttEntityActivePlan &plan)
{
	resetBucket(plan.tenSec);
	resetBucket(plan.oneMin);
	resetBucket(plan.fiveMin);
	resetBucket(plan.oneHour);
	resetBucket(plan.oneDay);
	resetBucket(plan.user);
	plan.activeCount = 0;
}

static BucketId
defaultBucketForIndex(size_t idx)
{
	if (idx >= kMqttEntityDescriptorCount) {
		return BucketId::Unknown;
	}
	return bucketIdFromFreq(kMqttEntities[idx].updateFreq);
}

static BucketId
bucketOverrideForIndex(size_t idx)
{
	for (size_t i = 0; i < g_runtime.overrideCount; ++i) {
		if (g_runtime.overrides[i].entityIndex == idx) {
			return g_runtime.overrides[i].bucketId;
		}
		if (g_runtime.overrides[i].entityIndex > idx) {
			break;
		}
	}
	return BucketId::Unknown;
}

static BucketId
bucketForIndex(size_t idx)
{
	BucketId bucket = bucketOverrideForIndex(idx);
	if (bucket != BucketId::Unknown) {
		return bucket;
	}
	return defaultBucketForIndex(idx);
}

static uint16_t *
allocateMembers(size_t count)
{
	if (count == 0) {
		return nullptr;
	}
	return new (std::nothrow) uint16_t[count];
}

static void
appendMember(MqttEntityActiveBucket &bucket, size_t &nextIndex, size_t idx, bool needsEssSnapshot)
{
	if (bucket.members != nullptr && nextIndex < bucket.count) {
		bucket.members[nextIndex] = static_cast<uint16_t>(idx);
	}
	++nextIndex;
	bucket.hasEssSnapshot = bucket.hasEssSnapshot || needsEssSnapshot;
}

static bool
rebuildActivePlan(void)
{
	MqttEntityActivePlan nextPlan{};

	for (size_t idx = 0; idx < kMqttEntityDescriptorCount; ++idx) {
		const bool needsEssSnapshot = kMqttEntities[idx].needsEssSnapshot;
		switch (bucketForIndex(idx)) {
		case BucketId::TenSec:
			nextPlan.tenSec.count++;
			nextPlan.tenSec.hasEssSnapshot = nextPlan.tenSec.hasEssSnapshot || needsEssSnapshot;
			nextPlan.activeCount++;
			break;
		case BucketId::OneMin:
			nextPlan.oneMin.count++;
			nextPlan.oneMin.hasEssSnapshot = nextPlan.oneMin.hasEssSnapshot || needsEssSnapshot;
			nextPlan.activeCount++;
			break;
		case BucketId::FiveMin:
			nextPlan.fiveMin.count++;
			nextPlan.fiveMin.hasEssSnapshot = nextPlan.fiveMin.hasEssSnapshot || needsEssSnapshot;
			nextPlan.activeCount++;
			break;
		case BucketId::OneHour:
			nextPlan.oneHour.count++;
			nextPlan.oneHour.hasEssSnapshot = nextPlan.oneHour.hasEssSnapshot || needsEssSnapshot;
			nextPlan.activeCount++;
			break;
		case BucketId::OneDay:
			nextPlan.oneDay.count++;
			nextPlan.oneDay.hasEssSnapshot = nextPlan.oneDay.hasEssSnapshot || needsEssSnapshot;
			nextPlan.activeCount++;
			break;
		case BucketId::User:
			nextPlan.user.count++;
			nextPlan.user.hasEssSnapshot = nextPlan.user.hasEssSnapshot || needsEssSnapshot;
			nextPlan.activeCount++;
			break;
		case BucketId::Disabled:
		case BucketId::Unknown:
		default:
			break;
		}
	}

	nextPlan.tenSec.members = allocateMembers(nextPlan.tenSec.count);
	nextPlan.oneMin.members = allocateMembers(nextPlan.oneMin.count);
	nextPlan.fiveMin.members = allocateMembers(nextPlan.fiveMin.count);
	nextPlan.oneHour.members = allocateMembers(nextPlan.oneHour.count);
	nextPlan.oneDay.members = allocateMembers(nextPlan.oneDay.count);
	nextPlan.user.members = allocateMembers(nextPlan.user.count);

	const bool allocFailed =
		(nextPlan.tenSec.count && nextPlan.tenSec.members == nullptr) ||
		(nextPlan.oneMin.count && nextPlan.oneMin.members == nullptr) ||
		(nextPlan.fiveMin.count && nextPlan.fiveMin.members == nullptr) ||
		(nextPlan.oneHour.count && nextPlan.oneHour.members == nullptr) ||
		(nextPlan.oneDay.count && nextPlan.oneDay.members == nullptr) ||
		(nextPlan.user.count && nextPlan.user.members == nullptr);
	if (allocFailed) {
		resetActivePlan(nextPlan);
		return false;
	}

	size_t tenSecIdx = 0;
	size_t oneMinIdx = 0;
	size_t fiveMinIdx = 0;
	size_t oneHourIdx = 0;
	size_t oneDayIdx = 0;
	size_t userIdx = 0;

	for (size_t idx = 0; idx < kMqttEntityDescriptorCount; ++idx) {
		const bool needsEssSnapshot = kMqttEntities[idx].needsEssSnapshot;
		switch (bucketForIndex(idx)) {
		case BucketId::TenSec:
			appendMember(nextPlan.tenSec, tenSecIdx, idx, needsEssSnapshot);
			break;
		case BucketId::OneMin:
			appendMember(nextPlan.oneMin, oneMinIdx, idx, needsEssSnapshot);
			break;
		case BucketId::FiveMin:
			appendMember(nextPlan.fiveMin, fiveMinIdx, idx, needsEssSnapshot);
			break;
		case BucketId::OneHour:
			appendMember(nextPlan.oneHour, oneHourIdx, idx, needsEssSnapshot);
			break;
		case BucketId::OneDay:
			appendMember(nextPlan.oneDay, oneDayIdx, idx, needsEssSnapshot);
			break;
		case BucketId::User:
			appendMember(nextPlan.user, userIdx, idx, needsEssSnapshot);
			break;
		case BucketId::Disabled:
		case BucketId::Unknown:
		default:
			break;
		}
	}

	resetActivePlan(g_runtime.plan);
	g_runtime.plan = nextPlan;
	g_runtime.planDirty = false;
	return true;
}

} // namespace

const mqttState *
mqttEntitiesDesc()
{
	return kMqttEntities;
}

const mqttState *
mqttEntityById(mqttEntityId id)
{
	for (size_t i = 0; i < kMqttEntityDescriptorCount; ++i) {
		if (kMqttEntities[i].entityId == id) {
			return &kMqttEntities[i];
		}
	}
	return nullptr;
}

size_t
mqttEntitiesCount()
{
	return kMqttEntityDescriptorCount;
}

bool
mqttEntityNameEquals(const mqttState *entity, const char *name)
{
	if (entity == nullptr || name == nullptr) {
		return false;
	}
#if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
	return strcmp_P(name, reinterpret_cast<PGM_P>(entity->mqttName)) == 0;
#else
	return strcmp(name, entity->mqttName) == 0;
#endif
}

void
mqttEntityNameCopy(const mqttState *entity, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return;
	}
	if (entity == nullptr || entity->mqttName == nullptr) {
		out[0] = '\0';
		return;
	}
#if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
	strncpy_P(out, reinterpret_cast<PGM_P>(entity->mqttName), outSize);
	out[outSize - 1] = '\0';
#else
	snprintf(out, outSize, "%s", entity->mqttName);
#endif
}

bool
mqttEntityNeedsEssSnapshotByIndex(size_t idx)
{
	if (idx >= kMqttEntityDescriptorCount) {
		return false;
	}
	return kMqttEntities[idx].needsEssSnapshot;
}

BucketId
mqttEntityBucketByIndex(size_t idx)
{
	if (!g_runtime.initialized || idx >= kMqttEntityDescriptorCount) {
		return BucketId::Unknown;
	}
	return bucketForIndex(idx);
}

mqttUpdateFreq
mqttEntityEffectiveFreqByIndex(size_t idx)
{
	return bucketIdToFreq(mqttEntityBucketByIndex(idx));
}

bool
mqttEntityCopyBuckets(BucketId *outBuckets, size_t entityCount)
{
	if (!g_runtime.initialized || outBuckets == nullptr || entityCount != kMqttEntityDescriptorCount) {
		return false;
	}
	for (size_t i = 0; i < entityCount; ++i) {
		outBuckets[i] = bucketForIndex(i);
	}
	return true;
}

bool
mqttEntityApplyBuckets(const BucketId *buckets, size_t entityCount)
{
	if (!g_runtime.initialized || buckets == nullptr || entityCount != kMqttEntityDescriptorCount) {
		return false;
	}

	size_t nextOverrideCount = 0;
	for (size_t i = 0; i < entityCount; ++i) {
		const BucketId bucket = buckets[i];
		if (bucket == BucketId::Unknown) {
			return false;
		}
		if (bucket != defaultBucketForIndex(i)) {
			nextOverrideCount++;
		}
	}

	MqttEntityBucketOverride *nextOverrides = nullptr;
	if (nextOverrideCount > 0) {
		nextOverrides = new (std::nothrow) MqttEntityBucketOverride[nextOverrideCount];
		if (nextOverrides == nullptr) {
			return false;
		}
		size_t nextIdx = 0;
		for (size_t i = 0; i < entityCount; ++i) {
			const BucketId bucket = buckets[i];
			if (bucket == defaultBucketForIndex(i)) {
				continue;
			}
			nextOverrides[nextIdx].entityIndex = static_cast<uint16_t>(i);
			nextOverrides[nextIdx].bucketId = bucket;
			nextIdx++;
		}
	}

	MqttEntityBucketOverride *oldOverrides = g_runtime.overrides;
	const size_t oldOverrideCount = g_runtime.overrideCount;
	const bool oldPlanDirty = g_runtime.planDirty;

	g_runtime.overrides = nextOverrides;
	g_runtime.overrideCount = nextOverrideCount;
	g_runtime.planDirty = true;
	if (!rebuildActivePlan()) {
		delete[] g_runtime.overrides;
		g_runtime.overrides = oldOverrides;
		g_runtime.overrideCount = oldOverrideCount;
		g_runtime.planDirty = oldPlanDirty;
		return false;
	}

	delete[] oldOverrides;
	return true;
}

const MqttEntityActivePlan *
mqttActivePlan()
{
	if (!g_runtime.initialized) {
		return nullptr;
	}
	if (g_runtime.planDirty) {
		if (!rebuildActivePlan()) {
			return nullptr;
		}
	}
	return &g_runtime.plan;
}

bool
mqttEntitiesRtAvailable()
{
	return g_runtime.initialized;
}

void
initMqttEntitiesRtIfNeeded(bool mqttEnabled)
{
	if (!mqttEnabled || g_runtime.initialized) {
		return;
	}
	g_runtime.initialized = true;
	g_runtime.planDirty = true;
}
