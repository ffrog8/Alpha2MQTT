// Purpose: Shared helpers for persisted polling bucket configuration.
// Responsibilities: Map stored values to runtime bucket assignments and parse bucket maps.
// Invariants: No dynamic allocation outside of small, bounded helpers.
#pragma once

#include <cstddef>
#include <cstdint>

#include "BucketScheduler.h"
#include "MqttEntities.h"

BucketId bucketIdFromLegacyFreq(int storedValue);
void applyBucketToRuntime(MqttEntityRuntime &rt, BucketId bucket);
// Entity identifier for persisted mappings is the MQTT name string (mqttName).
const mqttState *lookupEntityByName(const char *name,
                                    const mqttState *entities,
                                    size_t entityCount);

bool applyBucketMapString(const char *map,
                          const mqttState *entities,
                          size_t entityCount,
                          MqttEntityRuntime *rt,
                          uint32_t &unknownEntityCount,
                          uint32_t &invalidBucketCount,
                          uint32_t &duplicateEntityCount);
bool isValidMqttUpdateFreq(int value);
// Build a stable Bucket_Map string from legacy per-entity values.
bool buildBucketMapFromLegacy(const mqttState *entities,
                              size_t entityCount,
                              const int *storedValues,
                              char *out,
                              size_t outSize,
                              size_t &appliedCount);

// Build a stable Bucket_Map string from explicit bucket assignments (indexed by entity index).
// Returns true on success (even if appliedCount==0, which yields an empty map meaning "defaults").
bool buildBucketMapFromAssignments(const mqttState *entities,
                                   size_t entityCount,
                                   const BucketId *buckets,
                                   char *out,
                                   size_t outSize,
                                   size_t &appliedCount);
