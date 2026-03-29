// Purpose: Provide the flash-resident MQTT entity catalog and a sparse runtime
// selection model for enabled polling work.
// Responsibilities: Resolve per-entity bucket overrides, maintain enabled-only
// transaction plans plus entity fanout lists, and avoid per-catalog mutable
// allocations on ESP8266.
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

#if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
static const mqttState kMqttEntities[] PROGMEM = {
#else
static const mqttState kMqttEntities[] = {
#endif
#include "../include/MqttEntityCatalogRows.h"
};

#undef MQTT_ENTITY_ROW

static_assert(sizeof(kMqttEntities) / sizeof(kMqttEntities[0]) == kMqttEntityDescriptorCount,
              "kMqttEntityDescriptorCount must match kMqttEntities length");

static bool
copyEntityFromCatalog(size_t idx, mqttState *out)
{
	if (out == nullptr || idx >= kMqttEntityDescriptorCount) {
		return false;
	}
#if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
	memcpy_P(out, &kMqttEntities[idx], sizeof(*out));
#else
	*out = kMqttEntities[idx];
#endif
	return true;
}

static bool
isRetiredLegacyDispatchControlEntity(mqttEntityId entityId)
{
	switch (entityId) {
	case mqttEntityId::entityOpMode:
	case mqttEntityId::entitySocTarget:
	case mqttEntityId::entityChargePwr:
	case mqttEntityId::entityDischargePwr:
	case mqttEntityId::entityPushPwr:
	case mqttEntityId::entityDispatchDuration:
		return true;
	default:
		return false;
	}
}

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
	delete[] bucket.transactions;
	bucket.members = nullptr;
	bucket.transactions = nullptr;
	bucket.count = 0;
	bucket.transactionCount = 0;
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
	mqttState entity{};
	if (!copyEntityFromCatalog(idx, &entity)) {
		return BucketId::Unknown;
	}
	return bucketIdFromFreq(entity.updateFreq);
}

static bool
bucketMatchesStoredDefault(size_t idx, BucketId bucket)
{
	if (bucket == BucketId::Unknown) {
		return false;
	}
	mqttState entity{};
	if (!copyEntityFromCatalog(idx, &entity)) {
		return false;
	}
	if (entity.updateFreq == mqttUpdateFreq::freqNever) {
		// `freqNever` defaults render as Disabled in the portal, but dropping an
		// explicit Disabled override would resurrect discovery/state publishing.
		return false;
	}
	return bucket == bucketIdFromFreq(entity.updateFreq);
}

static BucketId
bucketOverrideForIndex(const MqttEntityBucketOverride *overrides, size_t overrideCount, size_t idx)
{
	for (size_t i = 0; i < overrideCount; ++i) {
		if (overrides[i].entityIndex == idx) {
			return overrides[i].bucketId;
		}
		if (overrides[i].entityIndex > idx) {
			break;
		}
	}
	return BucketId::Unknown;
}

static BucketId
bucketForIndex(const MqttEntityBucketOverride *overrides, size_t overrideCount, size_t idx)
{
	BucketId bucket = bucketOverrideForIndex(overrides, overrideCount, idx);
	if (bucket != BucketId::Unknown) {
		return bucket;
	}
	return defaultBucketForIndex(idx);
}

static BucketId
bucketForIndex(size_t idx)
{
	return bucketForIndex(g_runtime.overrides, g_runtime.overrideCount, idx);
}

static uint16_t *
allocateMembers(size_t count)
{
	if (count == 0) {
		return nullptr;
	}
	return new (std::nothrow) uint16_t[count];
}

static MqttPollTransactionKind
transactionKindForEntity(const mqttState &entity)
{
	if (entity.needsEssSnapshot) {
		return MqttPollTransactionKind::SnapshotFanout;
	}
	if (entity.readKind == MqttEntityReadKind::Register) {
		return MqttPollTransactionKind::RegisterFanout;
	}
	return MqttPollTransactionKind::SingleEntity;
}

struct TempTransactionSpec {
	MqttPollTransactionKind kind = MqttPollTransactionKind::SingleEntity;
	uint16_t readKey = 0;
	uint16_t firstMemberOffset = 0;
	uint16_t entityCount = 0;
};

static bool
transactionMatches(const TempTransactionSpec &spec, const mqttState &entity)
{
	const MqttPollTransactionKind kind = transactionKindForEntity(entity);
	if (spec.kind != kind) {
		return false;
	}
	switch (kind) {
	case MqttPollTransactionKind::SnapshotFanout:
		return true;
	case MqttPollTransactionKind::RegisterFanout:
		return spec.readKey == entity.readKey;
	case MqttPollTransactionKind::SingleEntity:
	default:
		return false;
	}
}

static bool
buildBucketTransactions(MqttEntityActiveBucket &bucket,
                       BucketId bucketId,
                       const MqttEntityBucketOverride *overrides,
                       size_t overrideCount)
{
	if (bucket.count == 0) {
		return true;
	}

	TempTransactionSpec *specs = new (std::nothrow) TempTransactionSpec[bucket.count];
	if (specs == nullptr) {
		return false;
	}

	uint16_t entityTxnIndex[kMqttEntityDescriptorCount];
	for (size_t i = 0; i < kMqttEntityDescriptorCount; ++i) {
		entityTxnIndex[i] = UINT16_MAX;
	}

	size_t txnCount = 0;
	size_t matchedEntityCount = 0;
	for (size_t idx = 0; idx < kMqttEntityDescriptorCount; ++idx) {
		if (bucketForIndex(overrides, overrideCount, idx) != bucketId) {
			continue;
		}
		mqttState entity{};
		if (!copyEntityFromCatalog(idx, &entity)) {
			delete[] specs;
			return false;
		}
		size_t txnIndex = txnCount;
		for (size_t existing = 0; existing < txnCount; ++existing) {
			if (transactionMatches(specs[existing], entity)) {
				txnIndex = existing;
				break;
			}
		}
		if (txnIndex == txnCount) {
			specs[txnIndex].kind = transactionKindForEntity(entity);
			specs[txnIndex].readKey = entity.readKey;
			txnCount++;
		}
		specs[txnIndex].entityCount++;
		entityTxnIndex[idx] = static_cast<uint16_t>(txnIndex);
		matchedEntityCount++;
	}

	if (matchedEntityCount != bucket.count) {
		delete[] specs;
		return false;
	}

	bucket.transactions = new (std::nothrow) MqttPollTransaction[txnCount];
	if (bucket.transactions == nullptr) {
		delete[] specs;
		return false;
	}

	size_t nextOffset = 0;
	for (size_t i = 0; i < txnCount; ++i) {
		specs[i].firstMemberOffset = static_cast<uint16_t>(nextOffset);
		bucket.transactions[i].firstMemberOffset = specs[i].firstMemberOffset;
		bucket.transactions[i].entityCount = specs[i].entityCount;
		bucket.transactions[i].readKey = specs[i].readKey;
		bucket.transactions[i].kind = specs[i].kind;
		nextOffset += specs[i].entityCount;
	}

	if (nextOffset != bucket.count) {
		delete[] specs;
		return false;
	}

	bucket.members = allocateMembers(bucket.count);
	if (bucket.count != 0 && bucket.members == nullptr) {
		delete[] specs;
		return false;
	}

	uint16_t fillCounts[kMqttEntityDescriptorCount];
	for (size_t i = 0; i < kMqttEntityDescriptorCount; ++i) {
		fillCounts[i] = 0;
	}

	for (size_t idx = 0; idx < kMqttEntityDescriptorCount; ++idx) {
		if (bucketForIndex(overrides, overrideCount, idx) != bucketId) {
			continue;
		}
		const uint16_t txnIndex = entityTxnIndex[idx];
		if (txnIndex == UINT16_MAX || txnIndex >= txnCount) {
			delete[] specs;
			return false;
		}
		const size_t offset = specs[txnIndex].firstMemberOffset + fillCounts[txnIndex];
		if (offset >= bucket.count) {
			delete[] specs;
			return false;
		}
		bucket.members[offset] = static_cast<uint16_t>(idx);
		fillCounts[txnIndex]++;
	}

	for (size_t i = 0; i < txnCount; ++i) {
		if (fillCounts[i] != specs[i].entityCount) {
			delete[] specs;
			return false;
		}
		if (specs[i].kind == MqttPollTransactionKind::SnapshotFanout) {
			bucket.hasEssSnapshot = true;
		}
	}

	bucket.transactionCount = txnCount;
	delete[] specs;
	return true;
}

static bool
rebuildActivePlanForOverrides(MqttEntityActivePlan &nextPlan,
                              const MqttEntityBucketOverride *overrides,
                              size_t overrideCount)
{
	for (size_t idx = 0; idx < kMqttEntityDescriptorCount; ++idx) {
		mqttState entity{};
		if (!copyEntityFromCatalog(idx, &entity)) {
			resetActivePlan(nextPlan);
			return false;
		}
		const bool needsEssSnapshot = entity.needsEssSnapshot;
		switch (bucketForIndex(overrides, overrideCount, idx)) {
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

	if (!buildBucketTransactions(nextPlan.tenSec, BucketId::TenSec, overrides, overrideCount) ||
	    !buildBucketTransactions(nextPlan.oneMin, BucketId::OneMin, overrides, overrideCount) ||
	    !buildBucketTransactions(nextPlan.fiveMin, BucketId::FiveMin, overrides, overrideCount) ||
	    !buildBucketTransactions(nextPlan.oneHour, BucketId::OneHour, overrides, overrideCount) ||
	    !buildBucketTransactions(nextPlan.oneDay, BucketId::OneDay, overrides, overrideCount) ||
	    !buildBucketTransactions(nextPlan.user, BucketId::User, overrides, overrideCount)) {
		resetActivePlan(nextPlan);
		return false;
	}
	return true;
}

static bool
rebuildActivePlan(void)
{
	MqttEntityActivePlan nextPlan{};
	if (!rebuildActivePlanForOverrides(nextPlan, g_runtime.overrides, g_runtime.overrideCount)) {
		return false;
	}
	resetActivePlan(g_runtime.plan);
	g_runtime.plan = nextPlan;
	g_runtime.planDirty = false;
	return true;
}

static bool
buildOverridesFromBuckets(const BucketId *buckets,
                          size_t entityCount,
                          MqttEntityBucketOverride *&nextOverrides,
                          size_t &nextOverrideCount)
{
	if (buckets == nullptr || entityCount != kMqttEntityDescriptorCount) {
		return false;
	}

	nextOverrideCount = 0;
	for (size_t i = 0; i < entityCount; ++i) {
		const BucketId bucket = buckets[i];
		if (bucket == BucketId::Unknown) {
			return false;
		}
		if (!bucketMatchesStoredDefault(i, bucket)) {
			nextOverrideCount++;
		}
	}

	nextOverrides = nullptr;
	if (nextOverrideCount == 0) {
		return true;
	}

	nextOverrides = new (std::nothrow) MqttEntityBucketOverride[nextOverrideCount];
	if (nextOverrides == nullptr) {
		return false;
	}

	size_t nextIdx = 0;
	for (size_t i = 0; i < entityCount; ++i) {
		const BucketId bucket = buckets[i];
		if (bucketMatchesStoredDefault(i, bucket)) {
			continue;
		}
		nextOverrides[nextIdx].entityIndex = static_cast<uint16_t>(i);
		nextOverrides[nextIdx].bucketId = bucket;
		nextIdx++;
	}
	return true;
}

} // namespace

const mqttState *
mqttEntitiesDesc()
{
#if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
	return nullptr;
#else
	return kMqttEntities;
#endif
}

const mqttState *
mqttEntityById(mqttEntityId id)
{
#if defined(ESP8266) || defined(ARDUINO_ARCH_ESP8266)
	return nullptr;
#else
	for (size_t i = 0; i < kMqttEntityDescriptorCount; ++i) {
		if (kMqttEntities[i].entityId == id) {
			return &kMqttEntities[i];
		}
	}
	return nullptr;
#endif
}

bool
mqttEntityCopyByIndex(size_t idx, mqttState *out)
{
	if (out == nullptr || idx >= kMqttEntityDescriptorCount) {
		return false;
	}
	return copyEntityFromCatalog(idx, out);
}

bool
mqttEntityCopyById(mqttEntityId id, mqttState *out)
{
	size_t idx = 0;
	if (!mqttEntityIndexById(id, &idx)) {
		return false;
	}
	return mqttEntityCopyByIndex(idx, out);
}

bool
mqttEntityIndexById(mqttEntityId id, size_t *outIdx)
{
	if (outIdx == nullptr) {
		return false;
	}
	for (size_t i = 0; i < kMqttEntityDescriptorCount; ++i) {
		mqttState entity{};
		if (!copyEntityFromCatalog(i, &entity)) {
			return false;
		}
		if (entity.entityId == id) {
			*outIdx = i;
			return true;
		}
	}
	return false;
}

bool
mqttEntityIndexByName(const char *name, size_t *outIdx)
{
	if (name == nullptr || name[0] == '\0' || outIdx == nullptr) {
		return false;
	}
	for (size_t i = 0; i < kMqttEntityDescriptorCount; ++i) {
		mqttState entity{};
		if (!copyEntityFromCatalog(i, &entity)) {
			return false;
		}
		if (mqttEntityNameEquals(&entity, name)) {
			*outIdx = i;
			return true;
		}
	}
	return false;
}

bool
mqttEntityCopyCatalog(mqttState *out, size_t count)
{
	if (out == nullptr || count != kMqttEntityDescriptorCount) {
		return false;
	}
	for (size_t i = 0; i < count; ++i) {
		if (!copyEntityFromCatalog(i, &out[i])) {
			return false;
		}
	}
	return true;
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
	mqttState entity{};
	if (!copyEntityFromCatalog(idx, &entity)) {
		return false;
	}
	return entity.needsEssSnapshot;
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
	if (idx >= kMqttEntityDescriptorCount) {
		return mqttUpdateFreq::freqDisabled;
	}
	mqttState entity{};
	if (!copyEntityFromCatalog(idx, &entity)) {
		return mqttUpdateFreq::freqDisabled;
	}
	if (!g_runtime.initialized) {
		return entity.updateFreq;
	}

	const BucketId overrideBucket = bucketOverrideForIndex(g_runtime.overrides, g_runtime.overrideCount, idx);
	if (overrideBucket == BucketId::Unknown && entity.updateFreq == mqttUpdateFreq::freqNever) {
		return mqttUpdateFreq::freqNever;
	}
	return bucketIdToFreq(bucketForIndex(idx));
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

	MqttEntityBucketOverride *nextOverrides = nullptr;
	size_t nextOverrideCount = 0;
	if (!buildOverridesFromBuckets(buckets, entityCount, nextOverrides, nextOverrideCount)) {
		delete[] nextOverrides;
		return false;
	}

	MqttEntityActivePlan nextPlan{};
	if (!rebuildActivePlanForOverrides(nextPlan, nextOverrides, nextOverrideCount)) {
		delete[] nextOverrides;
		return false;
	}

	delete[] g_runtime.overrides;
	g_runtime.overrides = nextOverrides;
	g_runtime.overrideCount = nextOverrideCount;
	resetActivePlan(g_runtime.plan);
	g_runtime.plan = nextPlan;
	g_runtime.planDirty = false;
	return true;
}

bool
mqttEntityCanApplyBuckets(const BucketId *buckets, size_t entityCount)
{
	if (!g_runtime.initialized || buckets == nullptr || entityCount != kMqttEntityDescriptorCount) {
		return false;
	}

	MqttEntityBucketOverride *nextOverrides = nullptr;
	size_t nextOverrideCount = 0;
	if (!buildOverridesFromBuckets(buckets, entityCount, nextOverrides, nextOverrideCount)) {
		delete[] nextOverrides;
		return false;
	}

	MqttEntityActivePlan nextPlan{};
	const bool ok = rebuildActivePlanForOverrides(nextPlan, nextOverrides, nextOverrideCount);
	resetActivePlan(nextPlan);
	delete[] nextOverrides;
	return ok;
}

bool
mqttEntityIncludedInPublicSurface(const mqttState *entity)
{
	return entity != nullptr && !isRetiredLegacyDispatchControlEntity(entity->entityId);
}

size_t
mqttEntityCompactPublicSurfaceAssignments(mqttState *entities, BucketId *buckets, size_t entityCount)
{
	if (entities == nullptr || buckets == nullptr) {
		return 0;
	}

	size_t writeIdx = 0;
	for (size_t readIdx = 0; readIdx < entityCount; ++readIdx) {
		if (!mqttEntityIncludedInPublicSurface(&entities[readIdx])) {
			continue;
		}
		if (writeIdx != readIdx) {
			entities[writeIdx] = entities[readIdx];
			buckets[writeIdx] = buckets[readIdx];
		}
		writeIdx++;
	}
	return writeIdx;
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
