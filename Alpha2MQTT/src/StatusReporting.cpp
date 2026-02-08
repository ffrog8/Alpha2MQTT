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
		"\"http_control_plane_enabled\":%s,"
		"\"ha_unique_id\":\"%s\""
		"}",
		snapshot.presence ? snapshot.presence : "",
		snapshot.a2mStatus ? snapshot.a2mStatus : "",
		snapshot.rs485Status ? snapshot.rs485Status : "",
		snapshot.gridStatus ? snapshot.gridStatus : "",
		snapshot.bootMode ? snapshot.bootMode : "",
		snapshot.bootIntent ? snapshot.bootIntent : "",
		snapshot.httpControlPlaneEnabled ? "true" : "false",
		snapshot.haUniqueId ? snapshot.haUniqueId : "");
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
		"\"rs485_backend\":\"%s\","
		"\"rs485_stub_mode\":\"%s\","
		"\"rs485_stub_fail_remaining\":%lu,"
		"\"rs485_stub_writes\":%lu,"
		"\"rs485_stub_last_write_reg\":%u,"
		"\"rs485_stub_last_write_ms\":%lu,"
		"\"ess_snapshot_last_ok\":%s,"
		"\"ess_snapshot_attempts\":%lu,"
		"\"dispatch_last_run_ms\":%lu,"
		"\"dispatch_last_skip_reason\":\"%s\","
		"\"poll_interval_s\":%lu,"
		"\"sched_10s_last_run_ms\":%lu,"
		"\"sched_1m_last_run_ms\":%lu,"
		"\"sched_5m_last_run_ms\":%lu,"
		"\"sched_1h_last_run_ms\":%lu,"
		"\"sched_1d_last_run_ms\":%lu,"
		"\"sched_user_last_run_ms\":%lu,"
		"\"sched_10s_count\":%u,"
		"\"sched_1m_count\":%u,"
		"\"sched_5m_count\":%u,"
		"\"sched_1h_count\":%u,"
		"\"sched_1d_count\":%u,"
		"\"sched_user_count\":%u,"
		"\"persist_load_ok\":%lu,"
		"\"persist_load_err\":%lu,"
		"\"persist_unknown_entity_count\":%lu,"
		"\"persist_invalid_bucket_count\":%lu,"
		"\"persist_duplicate_entity_count\":%lu,"
		"\"poll_ok_count\":%lu,"
		"\"poll_err_count\":%lu,"
		"\"last_poll_ms\":%lu,"
		"\"last_ok_ts_ms\":%lu,"
		"\"last_err_ts_ms\":%lu,"
		"\"last_err_code\":%d,"
		"\"rs485_probe_last_attempt_ms\":%lu,"
		"\"rs485_probe_backoff_ms\":%lu"
		"}",
		snapshot.rs485Backend ? snapshot.rs485Backend : "",
		snapshot.rs485StubMode ? snapshot.rs485StubMode : "",
		static_cast<unsigned long>(snapshot.rs485StubFailRemaining),
		static_cast<unsigned long>(snapshot.rs485StubWriteCount),
		static_cast<unsigned>(snapshot.rs485StubLastWriteStartReg),
		static_cast<unsigned long>(snapshot.rs485StubLastWriteMs),
		snapshot.essSnapshotLastOk ? "true" : "false",
		static_cast<unsigned long>(snapshot.essSnapshotAttempts),
		static_cast<unsigned long>(snapshot.dispatchLastRunMs),
		snapshot.dispatchLastSkipReason ? snapshot.dispatchLastSkipReason : "",
		static_cast<unsigned long>(snapshot.pollIntervalSeconds),
		static_cast<unsigned long>(snapshot.schedTenSecLastRunMs),
		static_cast<unsigned long>(snapshot.schedOneMinLastRunMs),
		static_cast<unsigned long>(snapshot.schedFiveMinLastRunMs),
		static_cast<unsigned long>(snapshot.schedOneHourLastRunMs),
		static_cast<unsigned long>(snapshot.schedOneDayLastRunMs),
		static_cast<unsigned long>(snapshot.schedUserLastRunMs),
		static_cast<unsigned int>(snapshot.schedTenSecCount),
		static_cast<unsigned int>(snapshot.schedOneMinCount),
		static_cast<unsigned int>(snapshot.schedFiveMinCount),
		static_cast<unsigned int>(snapshot.schedOneHourCount),
		static_cast<unsigned int>(snapshot.schedOneDayCount),
		static_cast<unsigned int>(snapshot.schedUserCount),
		static_cast<unsigned long>(snapshot.persistLoadOk),
		static_cast<unsigned long>(snapshot.persistLoadErr),
		static_cast<unsigned long>(snapshot.persistUnknownEntityCount),
		static_cast<unsigned long>(snapshot.persistInvalidBucketCount),
		static_cast<unsigned long>(snapshot.persistDuplicateEntityCount),
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

bool
buildStatusStubJson(const StatusStubSnapshot &snapshot, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	int written = snprintf(
		out,
		outSize,
		"{"
		"\"stub_reads\":%lu,"
		"\"stub_writes\":%lu,"
		"\"stub_unknown_reads\":%lu,"
		"\"last_read_reg\":%u,"
		"\"last_fn\":%u,"
		"\"last_fail_reg\":%u,"
		"\"last_fail_fn\":%u,"
		"\"last_fail_type\":\"%s\","
		"\"latency_ms\":%u,"
		"\"strict_unknown\":%s,"
		"\"fail_every_n\":%lu,"
		"\"fail_for_ms\":%lu,"
		"\"flap_online_ms\":%lu,"
		"\"flap_offline_ms\":%lu,"
		"\"probe_attempts\":%lu,"
		"\"probe_success_after_n\":%lu,"
		"\"soc_step_x10_per_snapshot\":%d"
		"}",
		static_cast<unsigned long>(snapshot.stubReads),
		static_cast<unsigned long>(snapshot.stubWrites),
		static_cast<unsigned long>(snapshot.stubUnknownReads),
		static_cast<unsigned>(snapshot.lastReadStartReg),
		static_cast<unsigned>(snapshot.lastFn),
		static_cast<unsigned>(snapshot.lastFailStartReg),
		static_cast<unsigned>(snapshot.lastFailFn),
		snapshot.lastFailType ? snapshot.lastFailType : "",
		static_cast<unsigned>(snapshot.latencyMs),
		snapshot.strictUnknown ? "true" : "false",
		static_cast<unsigned long>(snapshot.failEveryN),
		static_cast<unsigned long>(snapshot.failForMs),
		static_cast<unsigned long>(snapshot.flapOnlineMs),
		static_cast<unsigned long>(snapshot.flapOfflineMs),
		static_cast<unsigned long>(snapshot.probeAttempts),
		static_cast<unsigned long>(snapshot.probeSuccessAfterN),
		static_cast<int>(snapshot.socStepX10PerSnapshot));
	if (written < 0 || static_cast<size_t>(written) >= outSize) {
		return false;
	}
	return true;
}
