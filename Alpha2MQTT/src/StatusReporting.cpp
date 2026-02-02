// Purpose: Implement status/event helpers used by MQTT reporting and host tests.
// Responsibilities: Map event codes, rate-limit publishes, and format status payloads.
// Invariants: No Arduino dependencies or dynamic allocations.
#include "../include/StatusReporting.h"

#include <cstdio>
#include <cstring>

const char *
eventCodeName(MqttEventCode code)
{
	switch (code) {
	case MqttEventCode::Rs485Timeout:
		return "RS485_TIMEOUT";
	case MqttEventCode::Rs485Crc:
		return "RS485_CRC";
	case MqttEventCode::ModbusFrame:
		return "MODBUS_FRAME";
	case MqttEventCode::PollOverrun:
		return "POLL_OVERRUN";
	case MqttEventCode::MqttDisconnect:
		return "MQTT_DISCONNECT";
	case MqttEventCode::WifiDisconnect:
		return "WIFI_DISCONNECT";
	case MqttEventCode::None:
	default:
		return "NONE";
	}
}

EventLimiter::EventLimiter()
{
	memset(_lastPublishMs, 0, sizeof(_lastPublishMs));
}

bool
EventLimiter::shouldPublish(MqttEventCode code, uint32_t nowMs, uint32_t minIntervalMs)
{
	uint8_t index = static_cast<uint8_t>(code);
	if (index >= static_cast<uint8_t>(MqttEventCode::MaxValue)) {
		return false;
	}
	uint32_t last = _lastPublishMs[index];
	if (last != 0 && (nowMs - last) < minIntervalMs) {
		return false;
	}
	_lastPublishMs[index] = nowMs;
	return true;
}

bool
buildStatusCoreJson(const StatusCoreSnapshot &snapshot, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	int written = snprintf(
		out,
		outSize,
		"{"
		"\"presence\":\"%s\","
		"\"a2mStatus\":\"%s\","
		"\"rs485Status\":\"%s\","
		"\"gridStatus\":\"%s\","
		"\"boot_mode\":\"%s\","
		"\"boot_intent\":\"%s\","
		"\"http_control_plane_enabled\":%s"
		"}",
		snapshot.presence ? snapshot.presence : "",
		snapshot.a2mStatus ? snapshot.a2mStatus : "",
		snapshot.rs485Status ? snapshot.rs485Status : "",
		snapshot.gridStatus ? snapshot.gridStatus : "",
		snapshot.bootMode ? snapshot.bootMode : "",
		snapshot.bootIntent ? snapshot.bootIntent : "",
		snapshot.httpControlPlaneEnabled ? "true" : "false");
	if (written < 0 || static_cast<size_t>(written) >= outSize) {
		return false;
	}
	return true;
}

bool
buildStatusNetJson(const StatusNetSnapshot &snapshot, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	int written = snprintf(
		out,
		outSize,
		"{"
		"\"uptime_s\":%lu,"
		"\"free_heap\":%lu,"
		"\"rssi_dbm\":%d,"
		"\"ip\":\"%s\","
		"\"ssid\":\"%s\","
		"\"mqtt_connected\":%s,"
		"\"mqtt_reconnects\":%lu,"
		"\"wifi_status\":\"%s\","
		"\"wifi_status_code\":%d,"
		"\"wifi_reconnects\":%lu"
		"}",
		static_cast<unsigned long>(snapshot.uptimeS),
		static_cast<unsigned long>(snapshot.freeHeap),
		snapshot.rssiDbm,
		snapshot.ip ? snapshot.ip : "",
		snapshot.ssid ? snapshot.ssid : "",
		snapshot.mqttConnected ? "true" : "false",
		static_cast<unsigned long>(snapshot.mqttReconnects),
		snapshot.wifiStatus ? snapshot.wifiStatus : "",
		snapshot.wifiStatusCode,
		static_cast<unsigned long>(snapshot.wifiReconnects));
	if (written < 0 || static_cast<size_t>(written) >= outSize) {
		return false;
	}
	return true;
}

bool
buildStatusPollJson(const StatusPollSnapshot &snapshot, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	int written = snprintf(
		out,
		outSize,
		"{"
		"\"poll_ok_count\":%lu,"
		"\"poll_err_count\":%lu,"
		"\"last_poll_ms\":%lu,"
		"\"last_ok_ts_ms\":%lu,"
		"\"last_err_ts_ms\":%lu,"
		"\"last_err_code\":%d,"
		"\"rs485_probe_last_attempt_ms\":%lu,"
		"\"rs485_probe_backoff_ms\":%lu"
		"}",
		static_cast<unsigned long>(snapshot.pollOkCount),
		static_cast<unsigned long>(snapshot.pollErrCount),
		static_cast<unsigned long>(snapshot.lastPollMs),
		static_cast<unsigned long>(snapshot.lastOkTsMs),
		static_cast<unsigned long>(snapshot.lastErrTsMs),
		snapshot.lastErrCode,
		static_cast<unsigned long>(snapshot.rs485ProbeLastAttemptMs),
		static_cast<unsigned long>(snapshot.rs485ProbeBackoffMs));
	if (written < 0 || static_cast<size_t>(written) >= outSize) {
		return false;
	}
	return true;
}
