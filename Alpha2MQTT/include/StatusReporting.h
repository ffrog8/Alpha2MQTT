// Purpose: Define lightweight MQTT status/event helpers shared across firmware and host tests.
// Responsibilities: Event code mapping, rate limiting, and status JSON building without Arduino deps.
// Invariants: Keep payloads small and stable; no dynamic allocation inside builders.
// Dependencies: Standard C headers only.
#pragma once

#include <cstdint>
#include <cstddef>

constexpr size_t kStatusPollBucketCount = 6;
constexpr size_t kStatusPowerSnapshotSubreadCount = 4;

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
	uint32_t _publishedMask = 0;
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
	uint32_t maxBlock;
	uint8_t fragPct;
	uint32_t minFreeHeap;
	uint32_t minMaxBlock;
	uint8_t maxFragPct;
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
	bool inverterReady;
	bool essSnapshotOk;
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
	uint32_t rs485ErrorCount;
	uint32_t rs485TransportErrorCount;
	uint32_t rs485OtherErrorCount;
	uint32_t lastPollMs;
	uint32_t lastOkTsMs;
	uint32_t lastErrTsMs;
	int lastErrCode;
	uint32_t rs485ProbeLastAttemptMs;
	uint32_t rs485ProbeBackoffMs;
	uint32_t rs485ConnectionEpoch;
	uint32_t rs485BaudConfigured;
	uint32_t rs485BaudActual;
	const char *rs485BaudSync;
	const char *rs485Backend;
	bool essSnapshotLastOk;
	uint32_t essSnapshotAttempts;
	uint32_t essPowerSnapshotLastBuildMs;
	uint32_t snapshotPublishSkipCount;
	const char *rs485StubMode;
	uint32_t rs485StubFailRemaining;
	uint32_t rs485StubWriteCount;
	uint16_t rs485StubLastWriteStartReg;
	uint16_t rs485StubLastWriteRegCount;
	uint32_t rs485StubLastWriteMs;
	uint32_t dispatchRequestQueuedMs;
	uint32_t dispatchLastRunMs;
	uint32_t dispatchWaitDueToSnapshotMs;
	uint32_t dispatchQueueCoalesceCount;
	uint32_t dispatchBlockCacheHitCount;
	uint32_t pvBlockCacheHitCount;
	uint32_t pvMeterCacheHitCount;
	const char *dispatchLastSkipReason;
	const char *worstPhase;
	uint32_t worstFreeHeapB;
	uint32_t worstMaxBlockB;
	uint8_t worstFragPct;
	uint32_t mqttMaxPayloadSeen;
	const char *mqttMaxPayloadKind;
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
	bool pollingBudgetExceeded;
	uint32_t pollingBudgetOverrunCount;
	uint32_t pollingBudgetUsedMs[kStatusPollBucketCount];
	uint32_t pollingBudgetLimitMs[kStatusPollBucketCount];
	uint16_t pollingBacklogCount[kStatusPollBucketCount];
	uint32_t pollingBacklogOldestAgeMs[kStatusPollBucketCount];
	uint32_t pollingLastFullCycleAgeMs[kStatusPollBucketCount];
};

struct StatusStubSnapshot {
	uint32_t stubReads;
	uint32_t stubWrites;
	uint32_t stubUnknownReads;
	uint16_t socX10;
	uint16_t lastReadStartReg;
	uint16_t lastReadRegCount;
	uint8_t lastFn;
	uint16_t lastFailStartReg;
	uint8_t lastFailFn;
	const char *lastFailType;
	uint16_t lastWriteFailStartReg;
	uint8_t lastWriteFailFn;
	const char *lastWriteFailType;
	uint16_t failRegister;
	const char *failType;
	uint16_t latencyMs;
	bool strictUnknown;
	bool failReads;
	bool failWrites;
	uint32_t failEveryN;
	uint32_t failForMs;
	uint32_t flapOnlineMs;
	uint32_t flapOfflineMs;
	uint32_t probeAttempts;
	uint32_t probeSuccessAfterN;
	int16_t socStepX10PerSnapshot;
};

struct StatusManualReadSnapshot {
	uint32_t seq;
	uint32_t tsMs;
	int32_t requestedReg;
	uint16_t observedReg;
	const char *value;
};

struct StatusBootMemSnapshot {
	uint64_t fwBuildTsMs;
	uint32_t tsMs;
	uint32_t heapPreWifi;
	uint32_t heapPostWifi;
	uint32_t heapPostMqtt;
	uint32_t heapPreRs485;
	uint32_t heapPostRs485;
};

struct StatusPowerSnapshotDiagSubreadSnapshot {
	uint16_t totalQ10;
	uint16_t waitQ10;
	uint16_t quietQ10;
	uint8_t attempts;
	uint8_t retries;
	uint8_t resultCode;
};

struct StatusPowerSnapshotDiagLastSnapshot {
	bool valid;
	uint8_t reasonCode;
	uint32_t tsMs;
	int32_t triggerLoadW;
	int32_t loadW;
	uint16_t totalQ10;
	bool confirmTriggered;
	uint8_t confirmSamples;
	bool confirmAccepted;
	uint8_t confirmSelectedIndex;
	StatusPowerSnapshotDiagSubreadSnapshot
		subreads[kStatusPowerSnapshotSubreadCount];
};

struct StatusPowerSnapshotDiagSubreadCountSnapshot {
	uint32_t retryCount;
	uint32_t timeoutCount;
	uint32_t invalidFrameCount;
	uint32_t slowCount;
	uint16_t maxTotalQ10;
};

struct StatusPowerSnapshotDiagCountsSnapshot {
	uint32_t interestingEvents;
	uint32_t invalidReadEvents;
	uint32_t negativeLoadEvents;
	uint32_t lowLoadEvents;
	uint32_t slowTotalEvents;
	uint32_t confirmTriggered;
	uint32_t confirmResolved;
	uint32_t confirmSkippedPublish;
	StatusPowerSnapshotDiagSubreadCountSnapshot
		subreads[kStatusPowerSnapshotSubreadCount];
};

bool buildStatusCoreJson(const StatusCoreSnapshot &snapshot, char *out, size_t outSize);
bool buildStatusNetJson(const StatusNetSnapshot &snapshot, char *out, size_t outSize);
bool buildStatusPollJson(const StatusPollSnapshot &snapshot, char *out, size_t outSize);
bool buildStatusPollJsonCompact(const StatusPollSnapshot &snapshot, char *out, size_t outSize);
bool buildStatusStubJson(const StatusStubSnapshot &snapshot, char *out, size_t outSize);
bool buildStatusManualReadJson(const StatusManualReadSnapshot &snapshot, char *out, size_t outSize);
bool buildStatusBootMemJson(const StatusBootMemSnapshot &snapshot, char *out, size_t outSize);
bool buildStatusPowerSnapshotDiagLastJson(const StatusPowerSnapshotDiagLastSnapshot &snapshot,
                                          char *out,
                                          size_t outSize);
bool buildStatusPowerSnapshotDiagCountsJson(const StatusPowerSnapshotDiagCountsSnapshot &snapshot,
                                            char *out,
                                            size_t outSize);
