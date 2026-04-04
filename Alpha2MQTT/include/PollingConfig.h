// Purpose: Shared helpers for persisted polling bucket configuration.
// Responsibilities: Parse stored bucket mappings, normalize assignments, and
// emit compact persisted override maps.
// Invariants: Helpers operate on bounded caller-provided buffers only.
#pragma once

#include <cstddef>
#include <cstdint>

#include "BucketScheduler.h"
#include "MqttEntities.h"

enum class PollingLoadFailureKind {
	Transient,
	PersistedBucketMapCorrupt,
};

BucketId bucketIdFromLegacyFreq(int storedValue);
bool isValidMqttUpdateFreq(int value);
bool bucketMapUsesDescriptorIndices(const char *map);
bool shouldReloadPollingConfigFromStorage(bool pendingConfigSet, bool configLoaded);
bool shouldResetPersistedPollingConfig(PollingLoadFailureKind failureKind);
bool shouldMarkRecoveredPollingConfigLoaded(PollingLoadFailureKind failureKind);
bool shouldAcceptRecoveredPollingConfig(PollingLoadFailureKind failureKind,
                                        bool persistedResetOk);
bool shouldTrustRecoveredPortalPollingConfig(PollingLoadFailureKind failureKind);
bool shouldTrustPortalPollingRuntimeCache(bool pollingConfigLoaded);
bool parseStrictUint32(const char *text, uint32_t maxValue, uint32_t &outValue);
bool isDisableAllBucketMap(const char *map);
bool copyDisableAllBucketMap(char *out, size_t outSize);

// Copy a length-delimited MQTT payload into a caller-owned text buffer and
// append NUL. Chose a shared helper so the larger config/set path can be
// unit-tested without executing the Arduino MQTT callback.
bool copyLengthDelimitedString(const char *src,
                               size_t length,
                               char *out,
                               size_t outSize);

// Entity identifier for config payloads is the MQTT name string.
const mqttState *lookupEntityByName(const char *name,
                                    const mqttState *entities,
                                    size_t entityCount);

using PollingConfigEntryVisitor = bool (*)(const char *key, const char *value, void *context);
using MutablePollingConfigEntryVisitor = bool (*)(const char *key, char *value, void *context);
using LegacyPollingValueReader = bool (*)(size_t index,
                                          const mqttState *entity,
                                          int defaultValue,
                                          int &storedValue,
                                          void *context);

// Visit the flat config/set payload used by polling config. Keys remain quoted
// JSON strings; values may be quoted strings or simple bare scalars so clients
// can round-trip standard numeric poll_interval_s payloads.
bool visitPollingConfigEntries(const char *payload,
                               char *valueScratch,
                               size_t valueScratchSize,
                               PollingConfigEntryVisitor visitor,
                               void *context);

// Variant for deferred config/set buffers that can be tokenized in-place to
// avoid borrowing a second large scratch buffer from the MQTT publish path.
bool visitMutablePollingConfigEntries(char *payload,
                                      MutablePollingConfigEntryVisitor visitor,
                                      void *context);

// Validate the polling config payload shape without applying side effects.
bool validatePollingConfigEntries(const char *payload,
                                  char *valueScratch,
                                  size_t valueScratchSize);

enum class PollingProfileLineKind : uint8_t {
	Ignore = 0,
	Header,
	PollInterval,
	Assignment,
};

struct PollingProfileLine {
	PollingProfileLineKind kind = PollingProfileLineKind::Ignore;
	uint32_t pollIntervalSeconds = 0;
	BucketId bucketId = BucketId::Unknown;
	char entityName[64]{};
};

// Build a line-oriented polling-profile payload that can be exported one line
// at a time and later parsed incrementally by the portal upload path.
bool buildPollingProfilePayload(const char *pollIntervalS,
                                const char *bucketMap,
                                char *out,
                                size_t outSize);

// Parse and validate a complete line-oriented polling-profile payload. The
// output bucket map is normalized back to the canonical semicolon form used by
// existing persistence helpers.
bool parsePollingProfilePayload(const char *payload,
                                char *valueScratch,
                                size_t valueScratchSize,
                                char *pollIntervalOut,
                                size_t pollIntervalOutSize,
                                char *bucketMapOut,
                                size_t bucketMapOutSize);

// Parse one line from the line-oriented polling-profile format. Blank lines and
// comments return Ignore so streaming upload handlers can skip them without
// holding the full file in memory.
bool parsePollingProfileLine(const char *line, PollingProfileLine &out);

// Build a JSON payload compatible with MQTT/config/set from optional polling
// parameters submitted by portal handlers.
bool buildPollingConfigSetPayload(const char *pollIntervalS,
                                 const char *bucketMap,
                                 char *out,
                                 size_t outSize);

// Apply a Bucket_Map string into the provided bucket assignments. Supports both
// "Entity_Name=bucket;" and compact "#<descriptor-index>=bucket;" tokens.
bool applyBucketMapString(const char *map,
                          const mqttState *entities,
                          size_t entityCount,
                          BucketId *buckets,
                          uint32_t &unknownEntityCount,
                          uint32_t &invalidBucketCount,
                          uint32_t &duplicateEntityCount);

// Apply a persisted legacy Bucket_Map that still uses compact #<index> tokens
// from the pre-catalog metadata order. This preserves upgraded assignments
// until the map can be rewritten using stable entity names.
bool applyLegacyBucketMapString(const char *map,
                                const mqttState *entities,
                                size_t entityCount,
                                BucketId *buckets,
                                uint32_t &unknownEntityCount,
                                uint32_t &invalidBucketCount,
                                uint32_t &duplicateEntityCount);

// Build a stable Bucket_Map string from legacy per-entity values.
bool buildBucketMapFromLegacy(const mqttState *entities,
                              size_t entityCount,
                              const int *storedValues,
                              char *out,
                              size_t outSize,
                              size_t &appliedCount);

// Build a stable Bucket_Map string by reading legacy per-entity values
// incrementally. This keeps upgrade-time migration off the ESP8266 task stack.
bool buildBucketMapFromLegacyReader(const mqttState *entities,
                                    size_t entityCount,
                                    LegacyPollingValueReader reader,
                                    void *context,
                                    char *out,
                                    size_t outSize,
                                    size_t &appliedCount);
bool legacyPollingOverridesExist(const mqttState *entities,
                                 size_t entityCount,
                                 LegacyPollingValueReader reader,
                                 void *context);

// Build a stable Bucket_Map string from explicit bucket assignments. Names are
// used instead of descriptor indices so persisted config survives catalog growth.
bool buildBucketMapFromAssignments(const mqttState *entities,
                                   size_t entityCount,
                                   const BucketId *buckets,
                                   char *out,
                                   size_t outSize,
                                   size_t &appliedCount);

// Estimate the persisted Bucket_Map payload size for the active non-default
// overrides. The returned length excludes the trailing NUL.
size_t estimateBucketMapFromAssignmentsLength(const mqttState *entities,
                                              size_t entityCount,
                                              const BucketId *buckets,
                                              size_t &appliedCount);

// Build one chunk of the active (non-disabled) bucket assignments. Unlike the
// persisted Bucket_Map, this includes defaults so MQTT config consumers can
// reconstruct the full live schedule. `nextIndex` returns the next descriptor
// position to resume from when the output buffer fills.
bool buildActiveBucketMapChunkFromAssignments(const mqttState *entities,
                                              size_t entityCount,
                                              const BucketId *buckets,
                                              size_t startIndex,
                                              char *out,
                                              size_t outSize,
                                              size_t &nextIndex,
                                              size_t &appliedCount);
