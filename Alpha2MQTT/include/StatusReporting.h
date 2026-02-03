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
	uint32_t dispatchLastRunMs;
	const char *dispatchLastSkipReason;
};

bool buildStatusCoreJson(const StatusCoreSnapshot &snapshot, char *out, size_t outSize);
bool buildStatusNetJson(const StatusNetSnapshot &snapshot, char *out, size_t outSize);
bool buildStatusPollJson(const StatusPollSnapshot &snapshot, char *out, size_t outSize);
