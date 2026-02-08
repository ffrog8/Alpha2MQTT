// Purpose: Implement helpers for persisted polling bucket configuration.
// Responsibilities: Apply stored bucket mappings to runtime entity state.
#include "../include/PollingConfig.h"

#include <cctype>
#include <cstdio>
#include <cstring>

bool
isValidMqttUpdateFreq(int value)
{
	return value >= mqttUpdateFreq::freqTenSec && value <= mqttUpdateFreq::freqDisabled;
}

BucketId
bucketIdFromLegacyFreq(int storedValue)
{
	if (!isValidMqttUpdateFreq(storedValue)) {
		return BucketId::Unknown;
	}
	return bucketIdFromFreq(static_cast<mqttUpdateFreq>(storedValue));
}

void
applyBucketToRuntime(MqttEntityRuntime &rt, BucketId bucket)
{
	if (bucket == BucketId::Unknown) {
		return;
	}
	rt.bucketId = bucket;
	rt.effectiveFreq = bucketIdToFreq(bucket);
}

const mqttState *
lookupEntityByName(const char *name, const mqttState *entities, size_t entityCount)
{
	if (name == nullptr || entities == nullptr) {
		return nullptr;
	}
	for (size_t i = 0; i < entityCount; i++) {
		if (!strcmp(name, entities[i].mqttName)) {
			return &entities[i];
		}
	}
	return nullptr;
}

bool
applyBucketMapString(const char *map,
                     const mqttState *entities,
                     size_t entityCount,
                     MqttEntityRuntime *rt,
                     uint32_t &unknownEntityCount,
                     uint32_t &invalidBucketCount,
                     uint32_t &duplicateEntityCount)
{
	if (map == nullptr || *map == '\0' || entities == nullptr || rt == nullptr || entityCount == 0) {
		return false;
	}
	if (entityCount > kMqttEntityMaxCount) {
		return false;
	}

	uint8_t seen[kMqttEntityMaxCount];
	memset(seen, 0, sizeof(seen));
	const char *cursor = map;
	while (*cursor) {
		while (*cursor && (*cursor == ';' || isspace(static_cast<unsigned char>(*cursor)))) {
			cursor++;
		}
		if (!*cursor) {
			break;
		}

		char name[64] = {0};
		char bucket[32] = {0};
		size_t nameIdx = 0;
		size_t bucketIdx = 0;

		while (*cursor && *cursor != '=' && *cursor != ';' && nameIdx < sizeof(name) - 1) {
			name[nameIdx++] = *cursor++;
		}
		name[nameIdx] = '\0';

		if (*cursor != '=') {
			return false;
		}
		cursor++;

		while (*cursor && *cursor != ';' && bucketIdx < sizeof(bucket) - 1) {
			bucket[bucketIdx++] = *cursor++;
		}
		bucket[bucketIdx] = '\0';

		if (name[0] == '\0' || bucket[0] == '\0') {
			return false;
		}

		const mqttState *entity = lookupEntityByName(name, entities, entityCount);
		if (entity == nullptr) {
			unknownEntityCount++;
		} else {
			const size_t idx = static_cast<size_t>(entity - entities);
			if (idx < entityCount) {
				BucketId bucketId = bucketIdFromString(bucket);
				if (bucketId == BucketId::Unknown) {
					invalidBucketCount++;
				} else {
					if (seen[idx]) {
						duplicateEntityCount++;
					}
					applyBucketToRuntime(rt[idx], bucketId);
					seen[idx] = 1;
				}
			}
		}
	}

	return true;
}

bool
buildBucketMapFromLegacy(const mqttState *entities,
                          size_t entityCount,
                          const int *storedValues,
                          char *out,
                          size_t outSize,
                          size_t &appliedCount)
{
	if (entities == nullptr || storedValues == nullptr || out == nullptr || outSize == 0 || entityCount == 0) {
		return false;
	}
	appliedCount = 0;
	out[0] = '\0';
	size_t used = 0;

	for (size_t i = 0; i < entityCount; ++i) {
		if (!isValidMqttUpdateFreq(storedValues[i])) {
			continue;
		}
		BucketId bucket = bucketIdFromLegacyFreq(storedValues[i]);
		BucketId defaultBucket = bucketIdFromFreq(entities[i].updateFreq);
		if (bucket == BucketId::Unknown || bucket == defaultBucket) {
			continue;
		}
		const char *bucketStr = bucketIdToString(bucket);
		const int needed = snprintf(out + used,
		                            outSize - used,
		                            "%s=%s;",
		                            entities[i].mqttName,
		                            bucketStr);
		if (needed < 0 || static_cast<size_t>(needed) >= (outSize - used)) {
			return false;
		}
		used += static_cast<size_t>(needed);
		appliedCount++;
	}

	return appliedCount > 0;
}
