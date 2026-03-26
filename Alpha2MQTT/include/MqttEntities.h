// Purpose: Keep the compiled MQTT entity catalog in flash and expose the
// minimal mutable runtime state needed for user-selected polling.
// Invariants: Descriptor count is derived from the catalog rows, and active
// runtime state stores transaction plans plus entity fanout lists only for
// currently enabled entities.
#pragma once

#include <cstddef>
#include <cstdint>

#include "Definitions.h"

struct MqttEntityBucketOverride {
	uint16_t entityIndex;
	BucketId bucketId;
};

enum class MqttPollTransactionKind : uint8_t {
	SnapshotFanout = 0,
	RegisterFanout,
	SingleEntity
};

struct MqttPollTransaction {
	uint16_t firstMemberOffset;
	uint16_t entityCount;
	uint16_t readKey;
	MqttPollTransactionKind kind;
};

struct MqttEntityActiveBucket {
	uint16_t *members;
	MqttPollTransaction *transactions;
	size_t count;
	size_t transactionCount;
	bool hasEssSnapshot;
};

struct MqttEntityActivePlan {
	MqttEntityActiveBucket tenSec;
	MqttEntityActiveBucket oneMin;
	MqttEntityActiveBucket fiveMin;
	MqttEntityActiveBucket oneHour;
	MqttEntityActiveBucket oneDay;
	MqttEntityActiveBucket user;
	size_t activeCount;
};

// Compile-time count derived from the shared descriptor catalog. Scratch buffers
// that still require a fixed bound should size from this exact value.
constexpr size_t kMqttEntityDescriptorCount =
0
#define MQTT_ENTITY_ROW(...) + 1
#include "MqttEntityCatalogRows.h"
#undef MQTT_ENTITY_ROW
;

// Host/test code can still inspect the raw descriptor table. Firmware paths
// should prefer copy helpers so ESP8266 builds can keep the catalog in flash.
const mqttState *mqttEntitiesDesc();
const mqttState *mqttEntityById(mqttEntityId id);
bool mqttEntityCopyByIndex(size_t idx, mqttState *out);
bool mqttEntityCopyById(mqttEntityId id, mqttState *out);
bool mqttEntityIndexById(mqttEntityId id, size_t *outIdx);
bool mqttEntityIndexByName(const char *name, size_t *outIdx);
bool mqttEntityCopyCatalog(mqttState *out, size_t count);
size_t mqttEntitiesCount();

bool mqttEntityNameEquals(const mqttState *entity, const char *name);
void mqttEntityNameCopy(const mqttState *entity, char *out, size_t outSize);

bool mqttEntityNeedsEssSnapshotByIndex(size_t idx);
BucketId mqttEntityBucketByIndex(size_t idx);
mqttUpdateFreq mqttEntityEffectiveFreqByIndex(size_t idx);
bool mqttEntityCopyBuckets(BucketId *outBuckets, size_t entityCount);
bool mqttEntityCanApplyBuckets(const BucketId *buckets, size_t entityCount);
bool mqttEntityApplyBuckets(const BucketId *buckets, size_t entityCount);

const MqttEntityActivePlan *mqttActivePlan();

bool mqttEntitiesRtAvailable();

// Initializes sparse runtime state only when MQTT is enabled. Allocation of
// overrides and active poll members remains demand-driven after init.
void initMqttEntitiesRtIfNeeded(bool mqttEnabled);
