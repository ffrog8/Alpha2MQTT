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

// Entity identifier for config payloads is the MQTT name string.
const mqttState *lookupEntityByName(const char *name,
                                    const mqttState *entities,
                                    size_t entityCount);

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
