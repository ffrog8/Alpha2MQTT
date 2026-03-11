// Purpose: Implement helpers for persisted polling bucket configuration.
// Responsibilities: Parse stored bucket mappings, normalize override strings,
// and keep the persisted representation compact as the catalog grows.
#include "../include/PollingConfig.h"

#include <cerrno>
#include <cctype>
#include <cstdlib>
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

const mqttState *
lookupEntityByName(const char *name, const mqttState *entities, size_t entityCount)
{
	if (name == nullptr || entities == nullptr) {
		return nullptr;
	}
	for (size_t i = 0; i < entityCount; i++) {
		if (mqttEntityNameEquals(&entities[i], name)) {
			return &entities[i];
		}
	}
	return nullptr;
}

static bool
resolveEntityToken(const char *token,
                   const mqttState *entities,
                   size_t entityCount,
                   size_t &resolvedIndex)
{
	if (token == nullptr || token[0] == '\0') {
		return false;
	}
	if (token[0] == '#') {
		char *endPtr = nullptr;
		errno = 0;
		unsigned long parsed = strtoul(token + 1, &endPtr, 10);
		if (errno != 0 || endPtr == token + 1 || *endPtr != '\0' || parsed >= entityCount) {
			return false;
		}
		resolvedIndex = static_cast<size_t>(parsed);
		return true;
	}

	const mqttState *entity = lookupEntityByName(token, entities, entityCount);
	if (entity == nullptr) {
		return false;
	}
	resolvedIndex = static_cast<size_t>(entity - entities);
	return true;
}

bool
applyBucketMapString(const char *map,
                     const mqttState *entities,
                     size_t entityCount,
                     BucketId *buckets,
                     uint32_t &unknownEntityCount,
                     uint32_t &invalidBucketCount,
                     uint32_t &duplicateEntityCount)
{
	if (map == nullptr || *map == '\0' || entities == nullptr || buckets == nullptr || entityCount == 0) {
		return false;
	}
	if (entityCount > kMqttEntityDescriptorCount) {
		return false;
	}

	BucketId staged[kMqttEntityDescriptorCount];
	memcpy(staged, buckets, entityCount * sizeof(BucketId));

	uint8_t seen[kMqttEntityDescriptorCount];
	memset(seen, 0, sizeof(seen));
	const char *cursor = map;
	while (*cursor) {
		while (*cursor && (*cursor == ';' || isspace(static_cast<unsigned char>(*cursor)))) {
			cursor++;
		}
		if (!*cursor) {
			break;
		}

		char token[64] = {0};
		char bucket[32] = {0};
		size_t tokenIdx = 0;
		size_t bucketIdx = 0;

		while (*cursor && *cursor != '=' && *cursor != ';' && tokenIdx < sizeof(token) - 1) {
			token[tokenIdx++] = *cursor++;
		}
		token[tokenIdx] = '\0';

		if (*cursor != '=') {
			return false;
		}
		cursor++;

		while (*cursor && *cursor != ';' && bucketIdx < sizeof(bucket) - 1) {
			bucket[bucketIdx++] = *cursor++;
		}
		bucket[bucketIdx] = '\0';

		if (token[0] == '\0' || bucket[0] == '\0') {
			return false;
		}

		size_t idx = 0;
		if (!resolveEntityToken(token, entities, entityCount, idx)) {
			unknownEntityCount++;
		} else {
			const BucketId bucketId = bucketIdFromString(bucket);
			if (bucketId == BucketId::Unknown) {
				invalidBucketCount++;
			} else {
				if (seen[idx]) {
					duplicateEntityCount++;
				}
				staged[idx] = bucketId;
				seen[idx] = 1;
			}
		}
	}

	memcpy(buckets, staged, entityCount * sizeof(BucketId));
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
		const BucketId bucket = bucketIdFromLegacyFreq(storedValues[i]);
		const BucketId defaultBucket = bucketIdFromFreq(entities[i].updateFreq);
		if (bucket == BucketId::Unknown || bucket == defaultBucket) {
			continue;
		}
		const char *bucketStr = bucketIdToString(bucket);
		const int needed = snprintf(out + used,
		                            outSize - used,
		                            "#%u=%s;",
		                            static_cast<unsigned>(i),
		                            bucketStr);
		if (needed < 0 || static_cast<size_t>(needed) >= (outSize - used)) {
			return false;
		}
		used += static_cast<size_t>(needed);
		appliedCount++;
	}

	return appliedCount > 0;
}

bool
buildBucketMapFromAssignments(const mqttState *entities,
                              size_t entityCount,
                              const BucketId *buckets,
                              char *out,
                              size_t outSize,
                              size_t &appliedCount)
{
	if (entities == nullptr || buckets == nullptr || out == nullptr || outSize == 0 || entityCount == 0) {
		return false;
	}

	appliedCount = 0;
	out[0] = '\0';
	size_t used = 0;

	for (size_t i = 0; i < entityCount; ++i) {
		const BucketId bucket = buckets[i];
		if (bucket == BucketId::Unknown) {
			continue;
		}
		const BucketId defaultBucket = bucketIdFromFreq(entities[i].updateFreq);
		if (bucket == defaultBucket) {
			continue;
		}
		const char *bucketStr = bucketIdToString(bucket);
		const int needed = snprintf(out + used,
		                            outSize - used,
		                            "#%u=%s;",
		                            static_cast<unsigned>(i),
		                            bucketStr);
		if (needed < 0 || static_cast<size_t>(needed) >= (outSize - used)) {
			return false;
		}
		used += static_cast<size_t>(needed);
		appliedCount++;
	}

	return true;
}
