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

// Visit the flat {"key":"value"} config/set payload used by polling config.
// The parser intentionally stays limited to quoted string keys/values with no
// escape support because that matches the existing firmware contract exactly.
bool visitPollingConfigEntries(const char *payload,
                               char *valueScratch,
                               size_t valueScratchSize,
                               PollingConfigEntryVisitor visitor,
                               void *context);

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

// Build a compact Bucket_Map string from explicit bucket assignments indexed by
// descriptor order. Returns true on success even when appliedCount==0.
bool buildBucketMapFromAssignments(const mqttState *entities,
                                   size_t entityCount,
                                   const BucketId *buckets,
                                   char *out,
                                   size_t outSize,
                                   size_t &appliedCount);
