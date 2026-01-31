// Purpose: Keep MQTT entity metadata in flash and allocate mutable per-entity
// runtime state only when MQTT is enabled, preserving heap for portal modes.
// Invariants: Runtime state is allocated at most once per boot and is never freed.
#pragma once

#include <cstddef>

#include "Definitions.h"

struct MqttEntityRuntime {
	mqttUpdateFreq defaultFreq;
	mqttUpdateFreq effectiveFreq;
};

const mqttState *mqttEntitiesDesc();
size_t mqttEntitiesCount();

bool mqttEntitiesRtAvailable();
MqttEntityRuntime *mqttEntitiesRt();

// Allocates runtime state only when mqttEnabled is true. Allocation happens once per boot.
void initMqttEntitiesRtIfNeeded(bool mqttEnabled);
