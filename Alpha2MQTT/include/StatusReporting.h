// Purpose: Define lightweight MQTT status/event helpers shared across firmware and host tests.
// Responsibilities: Event code mapping, rate limiting, and status JSON building without Arduino deps.
// Invariants: Keep payloads small and stable; no dynamic allocation inside builders.
// Dependencies: Standard C headers only.
#pragma once

#include <cstdint>
#include <cstddef>

enum class MqttEventCode : uint8_t {
	None = 0,
	Rs485Timeout = 1,
	Rs485Crc = 2,
	ModbusFrame = 3,
	PollOverrun = 4,
	MqttDisconnect = 5,
	WifiDisconnect = 6,
	MaxValue
};

const char *eventCodeName(MqttEventCode code);

class EventLimiter {
public:
	EventLimiter();

	bool shouldPublish(MqttEventCode code, uint32_t nowMs, uint32_t minIntervalMs);

private:
	uint32_t _lastPublishMs[static_cast<uint8_t>(MqttEventCode::MaxValue)];
};

struct StatusCoreSnapshot {
	const char *presence;
	const char *a2mStatus;
	const char *rs485Status;
	const char *gridStatus;
	const char *bootMode;
	const char *bootIntent;
	bool httpControlPlaneEnabled;
	const char *haUniqueId;
};

struct StatusNetSnapshot {
	uint32_t uptimeS;
	uint32_t freeHeap;
	int rssiDbm;
	const char *ssid;
	const char *ip;
	bool mqttConnected;
	uint32_t mqttReconnects;
	const char *wifiStatus;
	int wifiStatusCode;
	uint32_t wifiReconnects;
};

struct StatusPollSnapshot {
	const char *wifiStatus;
	int wifiStatusCode;
	uint32_t wifiReconnects;
	uint32_t heapFreeB;
	uint32_t heapMaxBlockB;
	uint8_t heapFragPct;
	uint8_t memLevel;
	uint8_t bootHeapLevel;
	uint8_t bootHeapStage;
	uint32_t bootHeapFreeB;
	uint32_t bootHeapMaxBlockB;
	uint8_t bootHeapFragPct;
	uint32_t pollOkCount;
	uint32_t pollErrCount;
	uint32_t lastPollMs;
	uint32_t lastOkTsMs;
	uint32_t lastErrTsMs;
	int lastErrCode;
	uint32_t rs485ProbeLastAttemptMs;
	uint32_t rs485ProbeBackoffMs;
	const char *rs485Backend;
	bool essSnapshotLastOk;
	uint32_t essSnapshotAttempts;
	const char *rs485StubMode;
	uint32_t rs485StubFailRemaining;
	uint32_t rs485StubWriteCount;
	uint16_t rs485StubLastWriteStartReg;
	uint32_t rs485StubLastWriteMs;
	uint32_t dispatchLastRunMs;
	const char *dispatchLastSkipReason;
	uint32_t pollIntervalSeconds;
	uint32_t schedTenSecLastRunMs;
	uint32_t schedOneMinLastRunMs;
	uint32_t schedFiveMinLastRunMs;
	uint32_t schedOneHourLastRunMs;
	uint32_t schedOneDayLastRunMs;
	uint32_t schedUserLastRunMs;
	uint16_t schedTenSecCount;
	uint16_t schedOneMinCount;
	uint16_t schedFiveMinCount;
	uint16_t schedOneHourCount;
	uint16_t schedOneDayCount;
	uint16_t schedUserCount;
	uint32_t persistLoadOk;
	uint32_t persistLoadErr;
	uint32_t persistUnknownEntityCount;
	uint32_t persistInvalidBucketCount;
	uint32_t persistDuplicateEntityCount;
};

struct StatusStubSnapshot {
	uint32_t stubReads;
	uint32_t stubWrites;
	uint32_t stubUnknownReads;
	uint16_t lastReadStartReg;
	uint8_t lastFn;
	uint16_t lastFailStartReg;
	uint8_t lastFailFn;
	const char *lastFailType;
	uint16_t latencyMs;
	bool strictUnknown;
	uint32_t failEveryN;
	uint32_t failForMs;
	uint32_t flapOnlineMs;
	uint32_t flapOfflineMs;
	uint32_t probeAttempts;
	uint32_t probeSuccessAfterN;
	int16_t socStepX10PerSnapshot;
};

bool buildStatusCoreJson(const StatusCoreSnapshot &snapshot, char *out, size_t outSize);
bool buildStatusNetJson(const StatusNetSnapshot &snapshot, char *out, size_t outSize);
bool buildStatusPollJson(const StatusPollSnapshot &snapshot, char *out, size_t outSize);
bool buildStatusStubJson(const StatusStubSnapshot &snapshot, char *out, size_t outSize);
