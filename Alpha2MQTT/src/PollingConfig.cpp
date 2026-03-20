// Purpose: Implement helpers for persisted polling bucket configuration.
// Responsibilities: Parse stored bucket mappings, normalize override strings,
// and keep the persisted representation compact as the catalog grows.
#include "../include/PollingConfig.h"

#include <cerrno>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>

namespace {
constexpr char kDisableAllBucketMap[] = "__all__=disabled;";
}

bool
isValidMqttUpdateFreq(int value)
{
	return value >= mqttUpdateFreq::freqTenSec && value <= mqttUpdateFreq::freqDisabled;
}

bool
copyLengthDelimitedString(const char *src, size_t length, char *out, size_t outSize)
{
	if (src == nullptr || out == nullptr || outSize == 0 || length == 0 || length >= outSize) {
		return false;
	}
	memcpy(out, src, length);
	out[length] = '\0';
	return true;
}

BucketId
bucketIdFromLegacyFreq(int storedValue)
{
	// Legacy Freq_* keys predate freqUser. Their persisted numeric values used
	// 5 for freqNever, so upgraded devices must remap that slot explicitly
	// instead of casting into the current enum ordering.
	switch (storedValue) {
	case 0:
		return BucketId::TenSec;
	case 1:
		return BucketId::OneMin;
	case 2:
		return BucketId::FiveMin;
	case 3:
		return BucketId::OneHour;
	case 4:
		return BucketId::OneDay;
	case 5:
		return BucketId::Disabled;
	default:
		break;
	}
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

bool
bucketMapUsesDescriptorIndices(const char *map)
{
	if (map == nullptr) {
		return false;
	}

	const char *cursor = map;
	while (*cursor != '\0') {
		while (*cursor != '\0' && (*cursor == ';' || isspace(static_cast<unsigned char>(*cursor)))) {
			cursor++;
		}
		if (*cursor == '\0') {
			return false;
		}
		if (*cursor == '#') {
			return true;
		}
		while (*cursor != '\0' && *cursor != ';') {
			cursor++;
		}
	}
	return false;
}

bool
shouldReloadPollingConfigFromStorage(bool pendingConfigSet, bool configLoaded)
{
	return !pendingConfigSet && !configLoaded;
}

bool
parseStrictUint32(const char *text, uint32_t maxValue, uint32_t &outValue)
{
	if (text == nullptr || text[0] == '\0') {
		return false;
	}

	uint32_t value = 0;
	for (const char *cursor = text; *cursor != '\0'; ++cursor) {
		if (!isdigit(static_cast<unsigned char>(*cursor))) {
			return false;
		}
		const uint32_t digit = static_cast<uint32_t>(*cursor - '0');
		if (value > ((maxValue - digit) / 10U)) {
			return false;
		}
		value = (value * 10U) + digit;
	}

	outValue = value;
	return true;
}

bool
copyDisableAllBucketMap(char *out, size_t outSize)
{
	if (out == nullptr || outSize <= strlen(kDisableAllBucketMap)) {
		return false;
	}
	memcpy(out, kDisableAllBucketMap, sizeof(kDisableAllBucketMap));
	return true;
}

static const char *
skipWhitespace(const char *cursor)
{
	while (cursor != nullptr && *cursor != '\0' && isspace(static_cast<unsigned char>(*cursor))) {
		cursor++;
	}
	return cursor;
}

bool
isDisableAllBucketMap(const char *map)
{
	if (map == nullptr) {
		return false;
	}
	const char *cursor = skipWhitespace(map);
	if (cursor == nullptr) {
		return false;
	}
	const size_t sentinelLen = strlen(kDisableAllBucketMap);
	if (strncmp(cursor, kDisableAllBucketMap, sentinelLen) != 0) {
		return false;
	}
	cursor += sentinelLen;
	cursor = skipWhitespace(cursor);
	return cursor != nullptr && *cursor == '\0';
}

static bool
copyQuotedString(const char *&cursor, char *out, size_t outSize)
{
	if (cursor == nullptr || out == nullptr || outSize == 0 || *cursor != '"') {
		return false;
	}

	size_t index = 0;
	cursor++;
	while (*cursor != '\0' && *cursor != '"') {
		if (index + 1 >= outSize) {
			return false;
		}
		out[index++] = *cursor++;
	}
	if (*cursor != '"') {
		return false;
	}
	out[index] = '\0';
	cursor++;
	return true;
}

static bool
copyBareValue(const char *&cursor, char *out, size_t outSize)
{
	if (cursor == nullptr || out == nullptr || outSize == 0 || *cursor == '\0' || *cursor == '"' ||
	    *cursor == ',' || *cursor == '}') {
		return false;
	}

	size_t index = 0;
	while (*cursor != '\0' && *cursor != ',' && *cursor != '}' &&
	       !isspace(static_cast<unsigned char>(*cursor))) {
		if (index + 1 >= outSize) {
			return false;
		}
		out[index++] = *cursor++;
	}
	if (index == 0) {
		return false;
	}
	out[index] = '\0';
	return true;
}

static bool
copyJsonValue(const char *&cursor, char *out, size_t outSize)
{
	if (cursor == nullptr) {
		return false;
	}
	if (*cursor == '"') {
		return copyQuotedString(cursor, out, outSize);
	}
	return copyBareValue(cursor, out, outSize);
}

static bool
terminateQuotedString(char *&cursor, char *&out)
{
	if (cursor == nullptr || *cursor != '"') {
		return false;
	}

	cursor++;
	out = cursor;
	while (*cursor != '\0' && *cursor != '"') {
		cursor++;
	}
	if (*cursor != '"') {
		return false;
	}
	*cursor = '\0';
	cursor++;
	return true;
}

static bool
terminateBareValue(char *&cursor, char *&out, char &delimiter)
{
	if (cursor == nullptr || *cursor == '\0' || *cursor == '"' || *cursor == ',' || *cursor == '}') {
		return false;
	}

	out = cursor;
	while (*cursor != '\0' && *cursor != ',' && *cursor != '}' &&
	       !isspace(static_cast<unsigned char>(*cursor))) {
		cursor++;
	}
	if (cursor == out) {
		return false;
	}
	if (*cursor == '\0') {
		return false;
	}

	delimiter = *cursor;
	*cursor = '\0';
	return true;
}

static bool
terminateJsonValue(char *&cursor, char *&out, char &delimiter)
{
	if (cursor == nullptr) {
		return false;
	}
	if (*cursor == '"') {
		delimiter = '\0';
		return terminateQuotedString(cursor, out);
	}
	return terminateBareValue(cursor, out, delimiter);
}

bool
visitPollingConfigEntries(const char *payload,
                          char *valueScratch,
                          size_t valueScratchSize,
                          PollingConfigEntryVisitor visitor,
                          void *context)
{
	if (payload == nullptr || valueScratch == nullptr || valueScratchSize == 0 || visitor == nullptr) {
		return false;
	}

	const char *cursor = skipWhitespace(payload);
	if (cursor == nullptr || *cursor != '{') {
		return false;
	}
	cursor++;

	while (true) {
		char key[64];

		cursor = skipWhitespace(cursor);
		if (cursor == nullptr || *cursor == '\0') {
			return false;
		}
		if (*cursor == '}') {
			return true;
		}
		if (!copyQuotedString(cursor, key, sizeof(key))) {
			return false;
		}
		cursor = skipWhitespace(cursor);
		if (cursor == nullptr || *cursor != ':') {
			return false;
		}
		cursor++;
		cursor = skipWhitespace(cursor);
		if (cursor == nullptr || !copyJsonValue(cursor, valueScratch, valueScratchSize)) {
			return false;
		}
		if (!visitor(key, valueScratch, context)) {
			return false;
		}

		cursor = skipWhitespace(cursor);
		if (cursor == nullptr || *cursor == '\0') {
			return false;
		}
		if (*cursor == ',') {
			cursor++;
			continue;
		}
		if (*cursor == '}') {
			return true;
		}
		return false;
	}
}

bool
visitMutablePollingConfigEntries(char *payload,
                                 MutablePollingConfigEntryVisitor visitor,
                                 void *context)
{
	if (payload == nullptr || visitor == nullptr) {
		return false;
	}

	char *cursor = const_cast<char *>(skipWhitespace(payload));
	if (cursor == nullptr || *cursor != '{') {
		return false;
	}
	cursor++;

	while (true) {
		char key[64];
		char *value = nullptr;
		char valueDelimiter = '\0';

		cursor = const_cast<char *>(skipWhitespace(cursor));
		if (cursor == nullptr || *cursor == '\0') {
			return false;
		}
		if (*cursor == '}') {
			return true;
		}
		const char *keyCursor = cursor;
		if (!copyQuotedString(keyCursor, key, sizeof(key))) {
			return false;
		}
		cursor = const_cast<char *>(skipWhitespace(keyCursor));
		if (cursor == nullptr || *cursor != ':') {
			return false;
		}
		cursor++;
		cursor = const_cast<char *>(skipWhitespace(cursor));
		if (cursor == nullptr || !terminateJsonValue(cursor, value, valueDelimiter)) {
			return false;
		}
		if (!visitor(key, value, context)) {
			return false;
		}
		if (valueDelimiter != '\0') {
			*cursor = valueDelimiter;
		}

		cursor = const_cast<char *>(skipWhitespace(cursor));
		if (cursor == nullptr || *cursor == '\0') {
			return false;
		}
		if (*cursor == ',') {
			cursor++;
			continue;
		}
		if (*cursor == '}') {
			return true;
		}
		return false;
	}
}

static bool
validatePollingConfigEntry(const char * /* key */, const char * /* value */, void * /* context */)
{
	return true;
}

bool
validatePollingConfigEntries(const char *payload, char *valueScratch, size_t valueScratchSize)
{
	return visitPollingConfigEntries(payload,
	                                 valueScratch,
	                                 valueScratchSize,
	                                 validatePollingConfigEntry,
	                                 nullptr);
}

bool
buildPollingConfigSetPayload(const char *pollIntervalS,
                            const char *bucketMap,
                            char *out,
                            size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}

	const bool includePoll = (pollIntervalS != nullptr && pollIntervalS[0] != '\0');
	const bool includeMap = (bucketMap != nullptr && bucketMap[0] != '\0');

	if (!includePoll && !includeMap) {
		if (outSize < 3) {
			return false;
		}
		out[0] = '{';
		out[1] = '}';
		out[2] = '\0';
		return true;
	}

	int written = 0;
	if (includePoll && includeMap) {
		written = snprintf(out,
		                  outSize,
		                  "{\"poll_interval_s\":\"%s\",\"bucket_map\":\"%s\"}",
		                  pollIntervalS,
		                  bucketMap);
	} else if (includePoll) {
		written = snprintf(out, outSize, "{\"poll_interval_s\":\"%s\"}", pollIntervalS);
	} else {
		written = snprintf(out, outSize, "{\"bucket_map\":\"%s\"}", bucketMap);
	}

	if (written < 0) {
		return false;
	}
	return static_cast<size_t>(written) < outSize;
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
	if (isDisableAllBucketMap(map)) {
		for (size_t i = 0; i < entityCount; ++i) {
			buckets[i] = BucketId::Disabled;
		}
		return true;
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
appendBucketMapOverride(const mqttState &entity,
                        BucketId bucket,
                        char *out,
                        size_t outSize,
                        size_t &used)
{
	const char *bucketStr = bucketIdToString(bucket);
	char entityName[64];
	mqttEntityNameCopy(&entity, entityName, sizeof(entityName));
	const int needed = snprintf(out + used,
	                            outSize - used,
	                            "%s=%s;",
	                            entityName,
	                            bucketStr);
	if (needed < 0 || static_cast<size_t>(needed) >= (outSize - used)) {
		return false;
	}
	used += static_cast<size_t>(needed);
	return true;
}

static bool
appendActiveBucketAssignment(const mqttState &entity,
                             BucketId bucket,
                             char *out,
                             size_t outSize,
                             size_t &used)
{
	if (bucket == BucketId::Unknown || bucket == BucketId::Disabled) {
		return true;
	}
	return appendBucketMapOverride(entity, bucket, out, outSize, used);
}

static bool
bucketMatchesPersistedDefault(const mqttState &entity, BucketId bucket)
{
	if (bucket == BucketId::Unknown) {
		return false;
	}
	if (entity.updateFreq == mqttUpdateFreq::freqNever) {
		// A saved Disabled override must survive round-trips for freqNever
		// descriptors; otherwise they silently revert to active discovery/state.
		return false;
	}
	return bucket == bucketIdFromFreq(entity.updateFreq);
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
		if (!appendBucketMapOverride(entities[i], bucket, out, outSize, used)) {
			return false;
		}
		appliedCount++;
	}

	return appliedCount > 0;
}

bool
buildBucketMapFromLegacyReader(const mqttState *entities,
                               size_t entityCount,
                               LegacyPollingValueReader reader,
                               void *context,
                               char *out,
                               size_t outSize,
                               size_t &appliedCount)
{
	if (entities == nullptr || reader == nullptr || out == nullptr || outSize == 0 || entityCount == 0) {
		return false;
	}

	appliedCount = 0;
	out[0] = '\0';
	size_t used = 0;

	for (size_t i = 0; i < entityCount; ++i) {
		const int defaultValue = static_cast<int>(entities[i].updateFreq);
		int storedValue = defaultValue;
		if (!reader(i, &entities[i], defaultValue, storedValue, context)) {
			return false;
		}
		if (!isValidMqttUpdateFreq(storedValue)) {
			continue;
		}
		const BucketId bucket = bucketIdFromLegacyFreq(storedValue);
		if (bucket == BucketId::Unknown) {
			continue;
		}
		const bool missingLegacyKey = (storedValue == defaultValue);
		if (bucketMatchesPersistedDefault(entities[i], bucket) ||
		    (missingLegacyKey &&
		     entities[i].updateFreq == mqttUpdateFreq::freqNever &&
		     bucket == BucketId::Disabled)) {
			continue;
		}
		if (!appendBucketMapOverride(entities[i], bucket, out, outSize, used)) {
			return false;
		}
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
			if (bucketMatchesPersistedDefault(entities[i], bucket)) {
				continue;
			}
		if (!appendBucketMapOverride(entities[i], bucket, out, outSize, used)) {
			return false;
		}
		appliedCount++;
	}

	return true;
}

bool
buildActiveBucketMapChunkFromAssignments(const mqttState *entities,
                                         size_t entityCount,
                                         const BucketId *buckets,
                                         size_t startIndex,
                                         char *out,
                                         size_t outSize,
                                         size_t &nextIndex,
                                         size_t &appliedCount)
{
	if (entities == nullptr || buckets == nullptr || out == nullptr || outSize == 0) {
		return false;
	}
	if (entityCount > kMqttEntityDescriptorCount) {
		return false;
	}

	out[0] = '\0';
	appliedCount = 0;
	nextIndex = entityCount;
	if (startIndex >= entityCount) {
		return true;
	}

	size_t used = 0;
	for (size_t i = startIndex; i < entityCount; ++i) {
		const BucketId bucket = buckets[i];
		if (bucket == BucketId::Unknown || bucket == BucketId::Disabled) {
			continue;
		}
		const size_t usedBefore = used;
		if (!appendActiveBucketAssignment(entities[i], bucket, out, outSize, used)) {
			if (appliedCount == 0) {
				return false;
			}
			used = usedBefore;
			out[used] = '\0';
			nextIndex = i;
			return true;
		}
		appliedCount++;
	}
	return true;
}
