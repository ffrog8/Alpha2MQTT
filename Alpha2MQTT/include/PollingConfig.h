// Purpose: Shared helpers for persisted polling bucket configuration.
// Responsibilities: Parse stored bucket mappings, normalize assignments, and
// emit compact persisted override maps.
// Invariants: Helpers operate on bounded caller-provided buffers only.
#pragma once

#include <cstddef>
#include <cstdint>

#include "BucketScheduler.h"
#include "MqttEntities.h"

BucketId bucketIdFromLegacyFreq(int storedValue);
bool isValidMqttUpdateFreq(int value);
bool bucketMapUsesDescriptorIndices(const char *map);
bool shouldReloadPollingConfigFromStorage(bool pendingConfigSet, bool configLoaded);
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

// Visit the flat {"key":"value"} config/set payload used by polling config.
// The parser intentionally stays limited to quoted string keys/values with no
// escape support because that matches the existing firmware contract exactly.
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

// Apply a Bucket_Map string into the provided bucket assignments. Supports both
// "Entity_Name=bucket;" and compact "#<descriptor-index>=bucket;" tokens.
bool applyBucketMapString(const char *map,
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

// Build a stable Bucket_Map string from explicit bucket assignments. Names are
// used instead of descriptor indices so persisted config survives catalog growth.
bool buildBucketMapFromAssignments(const mqttState *entities,
                                   size_t entityCount,
                                   const BucketId *buckets,
                                   char *out,
                                   size_t outSize,
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
