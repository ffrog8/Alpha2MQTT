// Purpose: Keep MQTT entity metadata in flash and allocate mutable per-entity
// runtime state only when MQTT is enabled, preserving heap for portal modes.
// Invariants: Runtime state is allocated at most once per boot and is never freed.
#pragma once

#include <cstddef>

#include "Definitions.h"

struct MqttEntityRuntime {
	mqttUpdateFreq defaultFreq;
	mqttUpdateFreq effectiveFreq;
	BucketId bucketId;
};

const mqttState *mqttEntitiesDesc();
size_t mqttEntitiesCount();
// Compile-time max for stack/static buffers used in polling config parsing.
constexpr size_t kMqttEntityMaxCount = 64;

// Read-only metadata stored alongside the descriptor table.
bool mqttEntityNeedsEssSnapshotByIndex(size_t idx);

bool mqttEntitiesRtAvailable();
MqttEntityRuntime *mqttEntitiesRt();

// Allocates runtime state only when mqttEnabled is true. Allocation happens once per boot.
void initMqttEntitiesRtIfNeeded(bool mqttEnabled);
