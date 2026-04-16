// Purpose: Implement status/event helpers used by MQTT reporting and host tests.
// Responsibilities: Map event codes, rate-limit publishes, and format status payloads.
// Invariants: No Arduino dependencies or dynamic allocations.
#include "../include/StatusReporting.h"
#include "../include/MemoryHealth.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

#if defined(ARDUINO)
#include <pgmspace.h>
#define A2M_FMT(str) PSTR(str)
#define A2M_SNPRINTF snprintf_P
#define A2M_VSNPRINTF vsnprintf_P
#else
#define A2M_FMT(str) str
#define A2M_SNPRINTF snprintf
#define A2M_VSNPRINTF vsnprintf
#endif

#ifndef RS485_STUB
#define RS485_STUB 0
#endif

namespace {

static const char *kPollBudgetBucketKeys[kStatusPollBucketCount] = {
	"10s", "1m", "5m", "1h", "1d", "usr"
};

static bool
appendEscapedJsonString(char *dest, size_t destSize, const char *src);

static bool
appendJsonf(char *dest, size_t destSize, size_t &used, const char *fmt, ...);

static bool
buildPollBudgetJson(const StatusPollSnapshot &snapshot, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}

	int written = A2M_SNPRINTF(
		out,
		outSize,
		A2M_FMT("\"poll_budget\":{\"k\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"],"
		        "\"x\":%s,\"c\":%lu,"
		        "\"u\":[%lu,%lu,%lu,%lu,%lu,%lu],"
		        "\"l\":[%lu,%lu,%lu,%lu,%lu,%lu],"
		        "\"b\":[%u,%u,%u,%u,%u,%u],"
		        "\"a\":[%lu,%lu,%lu,%lu,%lu,%lu],"
		        "\"f\":[%lu,%lu,%lu,%lu,%lu,%lu]}"),
		kPollBudgetBucketKeys[0],
		kPollBudgetBucketKeys[1],
		kPollBudgetBucketKeys[2],
		kPollBudgetBucketKeys[3],
		kPollBudgetBucketKeys[4],
		kPollBudgetBucketKeys[5],
		snapshot.pollingBudgetExceeded ? "true" : "false",
		static_cast<unsigned long>(snapshot.pollingBudgetOverrunCount),
		static_cast<unsigned long>(snapshot.pollingBudgetUsedMs[0]),
		static_cast<unsigned long>(snapshot.pollingBudgetUsedMs[1]),
		static_cast<unsigned long>(snapshot.pollingBudgetUsedMs[2]),
		static_cast<unsigned long>(snapshot.pollingBudgetUsedMs[3]),
		static_cast<unsigned long>(snapshot.pollingBudgetUsedMs[4]),
		static_cast<unsigned long>(snapshot.pollingBudgetUsedMs[5]),
		static_cast<unsigned long>(snapshot.pollingBudgetLimitMs[0]),
		static_cast<unsigned long>(snapshot.pollingBudgetLimitMs[1]),
		static_cast<unsigned long>(snapshot.pollingBudgetLimitMs[2]),
		static_cast<unsigned long>(snapshot.pollingBudgetLimitMs[3]),
		static_cast<unsigned long>(snapshot.pollingBudgetLimitMs[4]),
		static_cast<unsigned long>(snapshot.pollingBudgetLimitMs[5]),
		static_cast<unsigned>(snapshot.pollingBacklogCount[0]),
		static_cast<unsigned>(snapshot.pollingBacklogCount[1]),
		static_cast<unsigned>(snapshot.pollingBacklogCount[2]),
		static_cast<unsigned>(snapshot.pollingBacklogCount[3]),
		static_cast<unsigned>(snapshot.pollingBacklogCount[4]),
		static_cast<unsigned>(snapshot.pollingBacklogCount[5]),
		static_cast<unsigned long>(snapshot.pollingBacklogOldestAgeMs[0]),
		static_cast<unsigned long>(snapshot.pollingBacklogOldestAgeMs[1]),
		static_cast<unsigned long>(snapshot.pollingBacklogOldestAgeMs[2]),
		static_cast<unsigned long>(snapshot.pollingBacklogOldestAgeMs[3]),
		static_cast<unsigned long>(snapshot.pollingBacklogOldestAgeMs[4]),
		static_cast<unsigned long>(snapshot.pollingBacklogOldestAgeMs[5]),
		static_cast<unsigned long>(snapshot.pollingLastFullCycleAgeMs[0]),
		static_cast<unsigned long>(snapshot.pollingLastFullCycleAgeMs[1]),
		static_cast<unsigned long>(snapshot.pollingLastFullCycleAgeMs[2]),
		static_cast<unsigned long>(snapshot.pollingLastFullCycleAgeMs[3]),
		static_cast<unsigned long>(snapshot.pollingLastFullCycleAgeMs[4]),
		static_cast<unsigned long>(snapshot.pollingLastFullCycleAgeMs[5]));
	if (written < 0 || static_cast<size_t>(written) >= outSize) {
		return false;
	}
	return true;
}

static bool
buildPollBudgetJsonCompact(const StatusPollSnapshot &snapshot, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}

	int written = A2M_SNPRINTF(
		out,
		outSize,
		A2M_FMT("\"poll_budget\":{\"x\":%s,\"c\":%lu,"
		        "\"b\":[%u,%u,%u,%u,%u,%u],"
		        "\"a\":[%lu,%lu,%lu,%lu,%lu,%lu],"
		        "\"f\":[%lu,%lu,%lu,%lu,%lu,%lu]}"),
		snapshot.pollingBudgetExceeded ? "true" : "false",
		static_cast<unsigned long>(snapshot.pollingBudgetOverrunCount),
		static_cast<unsigned>(snapshot.pollingBacklogCount[0]),
		static_cast<unsigned>(snapshot.pollingBacklogCount[1]),
		static_cast<unsigned>(snapshot.pollingBacklogCount[2]),
		static_cast<unsigned>(snapshot.pollingBacklogCount[3]),
		static_cast<unsigned>(snapshot.pollingBacklogCount[4]),
		static_cast<unsigned>(snapshot.pollingBacklogCount[5]),
		static_cast<unsigned long>(snapshot.pollingBacklogOldestAgeMs[0]),
		static_cast<unsigned long>(snapshot.pollingBacklogOldestAgeMs[1]),
		static_cast<unsigned long>(snapshot.pollingBacklogOldestAgeMs[2]),
		static_cast<unsigned long>(snapshot.pollingBacklogOldestAgeMs[3]),
		static_cast<unsigned long>(snapshot.pollingBacklogOldestAgeMs[4]),
		static_cast<unsigned long>(snapshot.pollingBacklogOldestAgeMs[5]),
		static_cast<unsigned long>(snapshot.pollingLastFullCycleAgeMs[0]),
		static_cast<unsigned long>(snapshot.pollingLastFullCycleAgeMs[1]),
		static_cast<unsigned long>(snapshot.pollingLastFullCycleAgeMs[2]),
		static_cast<unsigned long>(snapshot.pollingLastFullCycleAgeMs[3]),
		static_cast<unsigned long>(snapshot.pollingLastFullCycleAgeMs[4]),
		static_cast<unsigned long>(snapshot.pollingLastFullCycleAgeMs[5]));
	if (written < 0 || static_cast<size_t>(written) >= outSize) {
		return false;
	}
	return true;
}

static bool
buildNetMemoryJson(const StatusNetSnapshot &snapshot, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}

	const int written = A2M_SNPRINTF(
		out,
		outSize,
		A2M_FMT("\"max_block\":%lu,\"frag_pct\":%u,\"min_free_heap\":%lu,\"min_max_block\":%lu,\"max_frag_pct\":%u"),
		static_cast<unsigned long>(snapshot.maxBlock),
		static_cast<unsigned>(snapshot.fragPct),
		static_cast<unsigned long>(snapshot.minFreeHeap),
		static_cast<unsigned long>(snapshot.minMaxBlock),
		static_cast<unsigned>(snapshot.maxFragPct));
	if (written < 0 || static_cast<size_t>(written) >= outSize) {
		return false;
	}
	return true;
}

static bool
buildRuntimeDiagJson(const StatusPollSnapshot &snapshot, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}

	char worstPhase[24];
	char mqttMaxPayloadKind[24];
	if (!appendEscapedJsonString(worstPhase, sizeof(worstPhase), snapshot.worstPhase) ||
	    !appendEscapedJsonString(mqttMaxPayloadKind, sizeof(mqttMaxPayloadKind), snapshot.mqttMaxPayloadKind)) {
		return false;
	}

	const int written = A2M_SNPRINTF(
		out,
		outSize,
		A2M_FMT("\"worst_phase\":\"%s\",\"worst_free_heap\":%lu,\"worst_max_block\":%lu,\"worst_frag_pct\":%u,"
		        "\"mqtt_max_payload_seen\":%lu,\"mqtt_max_payload_kind\":\"%s\""),
		worstPhase,
		static_cast<unsigned long>(snapshot.worstFreeHeapB),
		static_cast<unsigned long>(snapshot.worstMaxBlockB),
		static_cast<unsigned>(snapshot.worstFragPct),
		static_cast<unsigned long>(snapshot.mqttMaxPayloadSeen),
		mqttMaxPayloadKind);
	if (written < 0 || static_cast<size_t>(written) >= outSize) {
		return false;
	}
	return true;
}

} // namespace

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

uint8_t
clampStatusRawReadSize(uint16_t requestedBytes, uint16_t rawSize, size_t rawCapacity)
{
	uint16_t clamped = rawSize;
	if (rawCapacity < clamped) {
		clamped = static_cast<uint16_t>(rawCapacity);
	}
	// Raw-read requests declare the largest payload callers expect back. Clamp
	// to both the response buffer capacity and that request bound so malformed
	// frames cannot drive the JSON builder past valid response bytes.
	if (requestedBytes > 0 && clamped > requestedBytes) {
		clamped = requestedBytes;
	}
	if (clamped > UINT8_MAX) {
		clamped = UINT8_MAX;
	}
	return static_cast<uint8_t>(clamped);
}

EventLimiter::EventLimiter()
{
	memset(_lastPublishMs, 0, sizeof(_lastPublishMs));
	_publishedMask = 0;
}

bool
EventLimiter::shouldPublish(MqttEventCode code, uint32_t nowMs, uint32_t minIntervalMs)
{
	uint8_t index = static_cast<uint8_t>(code);
	if (index >= static_cast<uint8_t>(MqttEventCode::MaxValue)) {
		return false;
	}
	uint32_t mask = static_cast<uint32_t>(1u << index);
	uint32_t last = _lastPublishMs[index];
	if ((_publishedMask & mask) != 0 && (nowMs - last) < minIntervalMs) {
		return false;
	}
	_lastPublishMs[index] = nowMs;
	_publishedMask |= mask;
	return true;
}

bool
buildStatusCoreJson(const StatusCoreSnapshot &snapshot, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	char presence[32];
	char a2mStatus[32];
	char rs485Status[32];
	char gridStatus[32];
	char bootMode[32];
	char bootIntent[32];
	char haUniqueId[64];
	if (!appendEscapedJsonString(presence, sizeof(presence), snapshot.presence) ||
	    !appendEscapedJsonString(a2mStatus, sizeof(a2mStatus), snapshot.a2mStatus) ||
	    !appendEscapedJsonString(rs485Status, sizeof(rs485Status), snapshot.rs485Status) ||
	    !appendEscapedJsonString(gridStatus, sizeof(gridStatus), snapshot.gridStatus) ||
	    !appendEscapedJsonString(bootMode, sizeof(bootMode), snapshot.bootMode) ||
	    !appendEscapedJsonString(bootIntent, sizeof(bootIntent), snapshot.bootIntent) ||
	    !appendEscapedJsonString(haUniqueId, sizeof(haUniqueId), snapshot.haUniqueId)) {
		return false;
	}
	int written = A2M_SNPRINTF(
		out,
		outSize,
		A2M_FMT("{"
		        "\"presence\":\"%s\","
		        "\"a2mStatus\":\"%s\","
		        "\"rs485Status\":\"%s\","
		        "\"gridStatus\":\"%s\","
		        "\"boot_mode\":\"%s\","
		        "\"boot_intent\":\"%s\","
		        "\"http_control_plane_enabled\":%s,"
		        "\"ha_unique_id\":\"%s\""
		        "}"),
		presence,
		a2mStatus,
		rs485Status,
		gridStatus,
		bootMode,
		bootIntent,
		snapshot.httpControlPlaneEnabled ? "true" : "false",
		haUniqueId);
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
	char ip[64];
	char ssid[128];
	char wifiStatus[32];
	char netMemory[128];
	if (!appendEscapedJsonString(ip, sizeof(ip), snapshot.ip) ||
	    !appendEscapedJsonString(ssid, sizeof(ssid), snapshot.ssid) ||
	    !appendEscapedJsonString(wifiStatus, sizeof(wifiStatus), snapshot.wifiStatus) ||
	    !buildNetMemoryJson(snapshot, netMemory, sizeof(netMemory))) {
		return false;
	}
	int written = A2M_SNPRINTF(
		out,
		outSize,
		A2M_FMT("{"
		        "\"uptime_s\":%lu,"
		        "\"free_heap\":%lu,"
		        "%s,"
		        "\"rssi_dbm\":%d,"
		        "\"ip\":\"%s\","
		        "\"ssid\":\"%s\","
		        "\"mqtt_connected\":%s,"
		        "\"mqtt_reconnects\":%lu,"
		        "\"wifi_status\":\"%s\","
		        "\"wifi_status_code\":%d,"
		        "\"wifi_reconnects\":%lu"
		        "}"),
		static_cast<unsigned long>(snapshot.uptimeS),
		static_cast<unsigned long>(snapshot.freeHeap),
		netMemory,
		snapshot.rssiDbm,
		ip,
		ssid,
		snapshot.mqttConnected ? "true" : "false",
		static_cast<unsigned long>(snapshot.mqttReconnects),
		wifiStatus,
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
	out[0] = '\0';
	char rs485Backend[32];
	char rs485StubMode[32];
	char rs485BaudSync[24];
	char dispatchLastSkipReason[64];
	if (!appendEscapedJsonString(rs485Backend, sizeof(rs485Backend), snapshot.rs485Backend) ||
	    !appendEscapedJsonString(rs485StubMode, sizeof(rs485StubMode), snapshot.rs485StubMode) ||
	    !appendEscapedJsonString(rs485BaudSync, sizeof(rs485BaudSync), snapshot.rs485BaudSync) ||
	    !appendEscapedJsonString(dispatchLastSkipReason, sizeof(dispatchLastSkipReason), snapshot.dispatchLastSkipReason)) {
		return false;
	}

	size_t used = 0;
	if (!appendJsonf(
		    out,
		    outSize,
		    used,
		    "{"
		    "\"rs485_backend\":\"%s\","
		    "\"rs485_stub_mode\":\"%s\","
		    "\"rs485_stub_fail_remaining\":%lu,"
		    "\"rs485_stub_writes\":%lu,"
		    "\"rs485_stub_last_write_reg\":%u,"
		    "\"rs485_stub_last_write_reg_count\":%u,"
		    "\"rs485_stub_last_write_ms\":%lu,"
		    "\"dispatch_request_queued_ms\":%lu,"
		    "\"inverter_ready\":%s,"
		    "\"ess_snapshot_ok\":%s,"
		    "\"mem\":{\"f\":%lu,\"m\":%lu,\"g\":%u,\"l\":%u},"
		    "\"boot_mem\":{\"l\":%u,\"s\":%u,\"f\":%lu,\"m\":%lu,\"g\":%u},"
		    "\"ess_snapshot_last_ok\":%s,"
		    "\"ess_snapshot_attempts\":%lu,"
		    "\"ess_power_snapshot_last_build_ms\":%lu,"
		    "\"snapshot_publish_skip_count\":%lu,"
		    "\"dispatch_last_run_ms\":%lu,"
		    "\"dispatch_wait_due_to_snapshot_ms\":%lu,"
		    "\"dispatch_queue_coalesce_count\":%lu,"
		    "\"dispatch_block_cache_hit_count\":%lu,"
		    "\"pv_block_cache_hit_count\":%lu,"
		    "\"pv_meter_cache_hit_count\":%lu,"
		    "\"dispatch_last_skip_reason\":\"%s\",",
		    rs485Backend,
		    rs485StubMode,
		    static_cast<unsigned long>(snapshot.rs485StubFailRemaining),
		    static_cast<unsigned long>(snapshot.rs485StubWriteCount),
		    static_cast<unsigned>(snapshot.rs485StubLastWriteStartReg),
		    static_cast<unsigned>(snapshot.rs485StubLastWriteRegCount),
		    static_cast<unsigned long>(snapshot.rs485StubLastWriteMs),
		    static_cast<unsigned long>(snapshot.dispatchRequestQueuedMs),
		    snapshot.inverterReady ? "true" : "false",
		    snapshot.essSnapshotOk ? "true" : "false",
		    static_cast<unsigned long>(snapshot.heapFreeB),
		    static_cast<unsigned long>(snapshot.heapMaxBlockB),
		    static_cast<unsigned>(snapshot.heapFragPct),
		    static_cast<unsigned>(snapshot.memLevel),
		    static_cast<unsigned>(snapshot.bootHeapLevel),
		    static_cast<unsigned>(snapshot.bootHeapStage),
		    static_cast<unsigned long>(snapshot.bootHeapFreeB),
		    static_cast<unsigned long>(snapshot.bootHeapMaxBlockB),
		    static_cast<unsigned>(snapshot.bootHeapFragPct),
		    snapshot.essSnapshotLastOk ? "true" : "false",
		    static_cast<unsigned long>(snapshot.essSnapshotAttempts),
		    static_cast<unsigned long>(snapshot.essPowerSnapshotLastBuildMs),
		    static_cast<unsigned long>(snapshot.snapshotPublishSkipCount),
		    static_cast<unsigned long>(snapshot.dispatchLastRunMs),
		    static_cast<unsigned long>(snapshot.dispatchWaitDueToSnapshotMs),
		    static_cast<unsigned long>(snapshot.dispatchQueueCoalesceCount),
		    static_cast<unsigned long>(snapshot.dispatchBlockCacheHitCount),
		    static_cast<unsigned long>(snapshot.pvBlockCacheHitCount),
		    static_cast<unsigned long>(snapshot.pvMeterCacheHitCount),
		    dispatchLastSkipReason)) {
		return false;
	}
	if (!buildRuntimeDiagJson(snapshot, out + used, outSize - used)) {
		return false;
	}
	used += strlen(out + used);
	if (!appendJsonf(out, outSize, used, ",\"poll_interval_s\":%lu,", static_cast<unsigned long>(snapshot.pollIntervalSeconds))) {
		return false;
	}
#if RS485_STUB
	if (!appendJsonf(out,
	                 outSize,
	                 used,
	                 "\"s10_ms\":%lu,\"s60_ms\":%lu,\"s300_ms\":%lu,\"s3600_ms\":%lu,\"s86400_ms\":%lu,\"su_ms\":%lu,",
	                 static_cast<unsigned long>(snapshot.schedTenSecLastRunMs),
	                 static_cast<unsigned long>(snapshot.schedOneMinLastRunMs),
	                 static_cast<unsigned long>(snapshot.schedFiveMinLastRunMs),
	                 static_cast<unsigned long>(snapshot.schedOneHourLastRunMs),
	                 static_cast<unsigned long>(snapshot.schedOneDayLastRunMs),
	                 static_cast<unsigned long>(snapshot.schedUserLastRunMs))) {
		return false;
	}
#endif
	if (!appendJsonf(
		    out,
		    outSize,
		    used,
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
		    "\"persist_duplicate_entity_count\":%lu,",
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
		    static_cast<unsigned long>(snapshot.persistDuplicateEntityCount))) {
		return false;
	}
	if (!buildPollBudgetJson(snapshot, out + used, outSize - used)) {
		return false;
	}
	used += strlen(out + used);
	if (!appendJsonf(
		    out,
		    outSize,
		    used,
		    ",\"poll_ok_count\":%lu,"
		    "\"poll_err_count\":%lu,"
		    "\"rs485_error_count\":%lu,"
		    "\"rs485_transport_error_count\":%lu,"
		    "\"rs485_other_error_count\":%lu,"
		    "\"last_poll_ms\":%lu,"
		    "\"last_ok_ts_ms\":%lu,"
		    "\"last_err_ts_ms\":%lu,"
		    "\"last_err_code\":%d,"
		    "\"rs485_probe_last_attempt_ms\":%lu,"
		    "\"rs485_probe_backoff_ms\":%lu,"
		    "\"rs485_connection_epoch\":%lu,"
		    "\"rs485_baud_configured\":%lu,"
		    "\"rs485_baud_actual\":%lu,"
		    "\"rs485_baud_sync\":\"%s\"",
		    static_cast<unsigned long>(snapshot.pollOkCount),
		    static_cast<unsigned long>(snapshot.pollErrCount),
		    static_cast<unsigned long>(snapshot.rs485ErrorCount),
		    static_cast<unsigned long>(snapshot.rs485TransportErrorCount),
		    static_cast<unsigned long>(snapshot.rs485OtherErrorCount),
		    static_cast<unsigned long>(snapshot.lastPollMs),
		    static_cast<unsigned long>(snapshot.lastOkTsMs),
		    static_cast<unsigned long>(snapshot.lastErrTsMs),
		    snapshot.lastErrCode,
		    static_cast<unsigned long>(snapshot.rs485ProbeLastAttemptMs),
		    static_cast<unsigned long>(snapshot.rs485ProbeBackoffMs),
		    static_cast<unsigned long>(snapshot.rs485ConnectionEpoch),
		    static_cast<unsigned long>(snapshot.rs485BaudConfigured),
		    static_cast<unsigned long>(snapshot.rs485BaudActual),
		    rs485BaudSync)) {
		return false;
	}
#if defined(DEBUG_OVER_SERIAL)
	if (!appendJsonf(
		    out,
		    outSize,
		    used,
		    ",\"mem_thr\":[%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u]",
		    static_cast<unsigned>(kBoot0WarnFreeB),
		    static_cast<unsigned>(kBoot0WarnMaxBlockB),
		    static_cast<unsigned>(kBoot0CritFreeB),
		    static_cast<unsigned>(kBoot0CritMaxBlockB),
		    static_cast<unsigned>(kBootNWarnFreeB),
		    static_cast<unsigned>(kBootNWarnMaxBlockB),
		    static_cast<unsigned>(kBootNWarnFragPct),
		    static_cast<unsigned>(kBootNCritFreeB),
		    static_cast<unsigned>(kBootNCritMaxBlockB),
		    static_cast<unsigned>(kBootNCritFragPct),
		    static_cast<unsigned>(kRuntimeWarnFreeB),
		    static_cast<unsigned>(kRuntimeWarnMaxBlockB),
		    static_cast<unsigned>(kRuntimeWarnFragPct),
		    static_cast<unsigned>(kRuntimeCritFreeB),
		    static_cast<unsigned>(kRuntimeCritMaxBlockB),
		    static_cast<unsigned>(kRuntimeCritFragPct))) {
		return false;
	}
#endif
	if (!appendJsonf(out, outSize, used, "}")) {
		return false;
	}
	return true;
}

bool
buildStatusPollJsonCompact(const StatusPollSnapshot &snapshot, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	out[0] = '\0';
	char rs485Backend[32];
	char rs485StubMode[32];
	char rs485BaudSync[24];
	char dispatchLastSkipReason[64];
	if (!appendEscapedJsonString(rs485Backend, sizeof(rs485Backend), snapshot.rs485Backend) ||
	    !appendEscapedJsonString(rs485StubMode, sizeof(rs485StubMode), snapshot.rs485StubMode) ||
	    !appendEscapedJsonString(rs485BaudSync, sizeof(rs485BaudSync), snapshot.rs485BaudSync) ||
	    !appendEscapedJsonString(dispatchLastSkipReason, sizeof(dispatchLastSkipReason), snapshot.dispatchLastSkipReason)) {
		return false;
	}

	size_t used = 0;
	if (!appendJsonf(
		    out,
		    outSize,
		    used,
		    "{"
		    "\"rs485_backend\":\"%s\","
		    "\"rs485_stub_mode\":\"%s\","
		    "\"rs485_stub_fail_remaining\":%lu,"
		    "\"rs485_stub_writes\":%lu,"
		    "\"rs485_stub_last_write_reg\":%u,"
		    "\"rs485_stub_last_write_reg_count\":%u,"
		    "\"rs485_stub_last_write_ms\":%lu,"
		    "\"dispatch_request_queued_ms\":%lu,"
		    "\"inverter_ready\":%s,"
		    "\"ess_snapshot_ok\":%s,"
		    "\"ess_snapshot_last_ok\":%s,"
		    "\"ess_snapshot_attempts\":%lu,"
		    "\"dispatch_last_run_ms\":%lu,"
		    "\"dispatch_last_skip_reason\":\"%s\",",
		    rs485Backend,
		    rs485StubMode,
		    static_cast<unsigned long>(snapshot.rs485StubFailRemaining),
		    static_cast<unsigned long>(snapshot.rs485StubWriteCount),
		    static_cast<unsigned>(snapshot.rs485StubLastWriteStartReg),
		    static_cast<unsigned>(snapshot.rs485StubLastWriteRegCount),
		    static_cast<unsigned long>(snapshot.rs485StubLastWriteMs),
		    static_cast<unsigned long>(snapshot.dispatchRequestQueuedMs),
		    snapshot.inverterReady ? "true" : "false",
		    snapshot.essSnapshotOk ? "true" : "false",
		    snapshot.essSnapshotLastOk ? "true" : "false",
		    static_cast<unsigned long>(snapshot.essSnapshotAttempts),
		    static_cast<unsigned long>(snapshot.dispatchLastRunMs),
		    dispatchLastSkipReason)) {
		return false;
	}
	if (!buildRuntimeDiagJson(snapshot, out + used, outSize - used)) {
		return false;
	}
	used += strlen(out + used);
	if (!appendJsonf(out, outSize, used, ",\"poll_interval_s\":%lu,", static_cast<unsigned long>(snapshot.pollIntervalSeconds))) {
		return false;
	}
#if RS485_STUB
	if (!appendJsonf(out,
	                 outSize,
	                 used,
	                 "\"s10_ms\":%lu,\"s60_ms\":%lu,\"s300_ms\":%lu,\"s3600_ms\":%lu,\"s86400_ms\":%lu,\"su_ms\":%lu,",
	                 static_cast<unsigned long>(snapshot.schedTenSecLastRunMs),
	                 static_cast<unsigned long>(snapshot.schedOneMinLastRunMs),
	                 static_cast<unsigned long>(snapshot.schedFiveMinLastRunMs),
	                 static_cast<unsigned long>(snapshot.schedOneHourLastRunMs),
	                 static_cast<unsigned long>(snapshot.schedOneDayLastRunMs),
	                 static_cast<unsigned long>(snapshot.schedUserLastRunMs))) {
		return false;
	}
#endif
	if (!appendJsonf(
		    out,
		    outSize,
		    used,
		    "\"last_poll_ms\":%lu,"
		    "\"poll_ok_count\":%lu,"
		    "\"poll_err_count\":%lu,"
		    "\"rs485_error_count\":%lu,"
		    "\"rs485_transport_error_count\":%lu,"
		    "\"rs485_other_error_count\":%lu,",
		    static_cast<unsigned long>(snapshot.lastPollMs),
		    static_cast<unsigned long>(snapshot.pollOkCount),
		    static_cast<unsigned long>(snapshot.pollErrCount),
		    static_cast<unsigned long>(snapshot.rs485ErrorCount),
		    static_cast<unsigned long>(snapshot.rs485TransportErrorCount),
		    static_cast<unsigned long>(snapshot.rs485OtherErrorCount))) {
		return false;
	}
	if (!buildPollBudgetJsonCompact(snapshot, out + used, outSize - used)) {
		return false;
	}
	used += strlen(out + used);
	if (!appendJsonf(
		    out,
		    outSize,
		    used,
		    ",\"rs485_probe_last_attempt_ms\":%lu,"
		    "\"rs485_probe_backoff_ms\":%lu,"
		    "\"rs485_connection_epoch\":%lu,"
		    "\"rs485_baud_configured\":%lu,"
		    "\"rs485_baud_actual\":%lu,"
		    "\"rs485_baud_sync\":\"%s\"}",
		    static_cast<unsigned long>(snapshot.rs485ProbeLastAttemptMs),
		    static_cast<unsigned long>(snapshot.rs485ProbeBackoffMs),
		    static_cast<unsigned long>(snapshot.rs485ConnectionEpoch),
		    static_cast<unsigned long>(snapshot.rs485BaudConfigured),
		    static_cast<unsigned long>(snapshot.rs485BaudActual),
		    rs485BaudSync)) {
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
	char lastFailType[32];
	char lastWriteFailType[32];
	char failType[32];
	if (!appendEscapedJsonString(lastFailType, sizeof(lastFailType), snapshot.lastFailType)) {
		return false;
	}
	if (!appendEscapedJsonString(lastWriteFailType, sizeof(lastWriteFailType), snapshot.lastWriteFailType)) {
		return false;
	}
	if (!appendEscapedJsonString(failType, sizeof(failType), snapshot.failType)) {
		return false;
	}
	int written = A2M_SNPRINTF(
		out,
		outSize,
		A2M_FMT("{"
		        "\"stub_reads\":%lu,"
		        "\"stub_writes\":%lu,"
		        "\"stub_unknown_reads\":%lu,"
		        "\"soc_x10\":%u,"
		        "\"last_read_reg\":%u,"
		        "\"last_read_reg_count\":%u,"
		        "\"last_fn\":%u,"
		        "\"last_fail_reg\":%u,"
		        "\"last_fail_fn\":%u,"
		        "\"last_fail_type\":\"%s\","
		        "\"last_write_fail_reg\":%u,"
		        "\"last_write_fail_fn\":%u,"
		        "\"last_write_fail_type\":\"%s\","
		        "\"fail_reg\":%u,"
		        "\"fail_type\":\"%s\","
		        "\"latency_ms\":%u,"
		        "\"strict_unknown\":%s,"
		        "\"fail_reads\":%s,"
		        "\"fail_writes\":%s,"
		        "\"fail_every_n\":%lu,"
		        "\"fail_for_ms\":%lu,"
		        "\"flap_online_ms\":%lu,"
		        "\"flap_offline_ms\":%lu,"
		        "\"probe_attempts\":%lu,"
		        "\"probe_success_after_n\":%lu,"
		        "\"soc_step_x10_per_snapshot\":%d"
		        "}"),
		static_cast<unsigned long>(snapshot.stubReads),
		static_cast<unsigned long>(snapshot.stubWrites),
		static_cast<unsigned long>(snapshot.stubUnknownReads),
		static_cast<unsigned>(snapshot.socX10),
		static_cast<unsigned>(snapshot.lastReadStartReg),
		static_cast<unsigned>(snapshot.lastReadRegCount),
		static_cast<unsigned>(snapshot.lastFn),
		static_cast<unsigned>(snapshot.lastFailStartReg),
		static_cast<unsigned>(snapshot.lastFailFn),
		lastFailType,
		static_cast<unsigned>(snapshot.lastWriteFailStartReg),
		static_cast<unsigned>(snapshot.lastWriteFailFn),
		lastWriteFailType,
		static_cast<unsigned>(snapshot.failRegister),
		failType,
		static_cast<unsigned>(snapshot.latencyMs),
		snapshot.strictUnknown ? "true" : "false",
		snapshot.failReads ? "true" : "false",
		snapshot.failWrites ? "true" : "false",
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

namespace {

static bool
appendEscapedJsonString(char *dest, size_t destSize, const char *src)
{
	if (dest == nullptr || destSize == 0) {
		return false;
	}
	dest[0] = '\0';
	if (src == nullptr) {
		return true;
	}
	size_t writePos = 0;
	for (size_t i = 0; src[i] != '\0'; ++i) {
		const char ch = src[i];
		if ((ch == '\\') || (ch == '"')) {
			if ((writePos + 2) >= destSize) {
				return false;
			}
			dest[writePos++] = '\\';
			dest[writePos++] = ch;
			continue;
		}
		if (static_cast<unsigned char>(ch) < 0x20) {
			if ((writePos + 6) >= destSize) {
				return false;
			}
			const int written = A2M_SNPRINTF(dest + writePos, destSize - writePos, A2M_FMT("\\u%04x"), ch);
			if ((written <= 0) || (static_cast<size_t>(written) >= (destSize - writePos))) {
				return false;
			}
			writePos += static_cast<size_t>(written);
			continue;
		}
		if ((writePos + 1) >= destSize) {
			return false;
		}
		dest[writePos++] = ch;
	}
	dest[writePos] = '\0';
	return true;
}

static bool
appendJsonf(char *dest, size_t destSize, size_t &used, const char *fmt, ...)
{
	if (dest == nullptr || used >= destSize) {
		return false;
	}
	va_list args;
	va_start(args, fmt);
	const int written = A2M_VSNPRINTF(dest + used, destSize - used, fmt, args);
	va_end(args);
	if (written < 0 || static_cast<size_t>(written) >= (destSize - used)) {
		return false;
	}
	used += static_cast<size_t>(written);
	return true;
}

static bool
appendPowerSnapshotDiagSubreadJson(char *dest,
                                   size_t destSize,
                                   size_t &used,
                                   const char *key,
                                   const StatusPowerSnapshotDiagSubreadSnapshot &subread)
{
	if (key == nullptr) {
		return false;
	}
	char escapedResult[64];
	if (!appendEscapedJsonString(escapedResult, sizeof(escapedResult), subread.result != nullptr ? subread.result : "")) {
		return false;
	}
	return appendJsonf(dest,
	                   destSize,
	                   used,
	                   "\"%s\":{\"total_q10\":%u,\"wait_q10\":%u,\"quiet_q10\":%u,\"attempts\":%u,\"retries\":%u,"
	                   "\"result\":\"%s\"}",
	                   key,
	                   static_cast<unsigned>(subread.totalQ10),
	                   static_cast<unsigned>(subread.waitQ10),
	                   static_cast<unsigned>(subread.quietQ10),
	                   static_cast<unsigned>(subread.attempts),
	                   static_cast<unsigned>(subread.retries),
	                   escapedResult);
}

static bool
appendPowerSnapshotDiagCounterJson(char *dest,
                                   size_t destSize,
                                   size_t &used,
                                   const char *key,
                                   const StatusPowerSnapshotDiagCounterSubreadSnapshot &counter)
{
	if (key == nullptr) {
		return false;
	}
	return appendJsonf(dest,
	                   destSize,
	                   used,
	                   "\"%s\":{\"slow\":%lu,\"retry\":%lu,\"timeout\":%lu,\"invalid_frame\":%lu,\"max_total_q10\":%u}",
	                   key,
	                   static_cast<unsigned long>(counter.slowCount),
	                   static_cast<unsigned long>(counter.retryCount),
	                   static_cast<unsigned long>(counter.timeoutCount),
	                   static_cast<unsigned long>(counter.invalidFrameCount),
	                   static_cast<unsigned>(counter.maxTotalQ10));
}

} // namespace

bool
buildStatusManualReadJson(const StatusManualReadSnapshot &snapshot, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	char escapedValue[128];
	if (!appendEscapedJsonString(escapedValue, sizeof(escapedValue), snapshot.value)) {
		return false;
	}
	const int written = A2M_SNPRINTF(
		out,
		outSize,
		A2M_FMT("{"
		        "\"seq\":%lu,"
		        "\"ts_ms\":%lu,"
		        "\"requested_reg\":%ld,"
		        "\"observed_reg\":%u,"
		        "\"value\":\"%s\""
		        "}"),
		static_cast<unsigned long>(snapshot.seq),
		static_cast<unsigned long>(snapshot.tsMs),
		static_cast<long>(snapshot.requestedReg),
		static_cast<unsigned>(snapshot.observedReg),
		escapedValue);
	if (written < 0 || static_cast<size_t>(written) >= outSize) {
		return false;
	}
	return true;
}

bool
buildStatusRawReadJson(const StatusRawReadSnapshot &snapshot, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	if (snapshot.rawSize > 0 && snapshot.raw == nullptr) {
		return false;
	}
	char escapedStatus[64];
	if (!appendEscapedJsonString(escapedStatus, sizeof(escapedStatus), snapshot.status)) {
		return false;
	}
	out[0] = '\0';
	size_t used = 0;
	if (!appendJsonf(out,
	                 outSize,
	                 used,
	                 "{"
	                 "\"seq\":%lu,"
	                 "\"ts_ms\":%lu,"
	                 "\"requested_reg\":%ld,"
	                 "\"requested_bytes\":%u,"
	                 "\"function_code\":%u,"
	                 "\"status\":\"%s\","
	                 "\"raw_size\":%u,"
	                 "\"raw\":[",
	                 static_cast<unsigned long>(snapshot.seq),
	                 static_cast<unsigned long>(snapshot.tsMs),
	                 static_cast<long>(snapshot.requestedReg),
	                 static_cast<unsigned>(snapshot.requestedBytes),
	                 static_cast<unsigned>(snapshot.functionCode),
	                 escapedStatus,
	                 static_cast<unsigned>(snapshot.rawSize))) {
		return false;
	}
	for (uint8_t i = 0; i < snapshot.rawSize; ++i) {
		if (!appendJsonf(out,
		                 outSize,
		                 used,
		                 (i == 0) ? "%u" : ",%u",
		                 static_cast<unsigned>(snapshot.raw[i]))) {
			return false;
		}
	}
	if (!appendJsonf(out, outSize, used, "]")) {
		return false;
	}
	if (snapshot.hasSlaveErrorCode &&
	    !appendJsonf(out,
	                 outSize,
	                 used,
	                 ",\"slave_error_code\":%u",
	                 static_cast<unsigned>(snapshot.slaveErrorCode))) {
		return false;
	}
	if (!appendJsonf(out, outSize, used, "}")) {
		return false;
	}
	return true;
}

bool
buildStatusBootMemJson(const StatusBootMemSnapshot &snapshot, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	const int written = A2M_SNPRINTF(
		out,
		outSize,
		A2M_FMT("{"
		        "\"fw_build_ts_ms\":%llu,"
		        "\"ts_ms\":%lu,"
		        "\"heap_pre_wifi\":%lu,"
		        "\"heap_post_wifi\":%lu,"
		        "\"heap_post_mqtt\":%lu,"
		        "\"heap_pre_rs485\":%lu,"
		        "\"heap_post_rs485\":%lu"
		        "}"),
		static_cast<unsigned long long>(snapshot.fwBuildTsMs),
		static_cast<unsigned long>(snapshot.tsMs),
		static_cast<unsigned long>(snapshot.heapPreWifi),
		static_cast<unsigned long>(snapshot.heapPostWifi),
		static_cast<unsigned long>(snapshot.heapPostMqtt),
		static_cast<unsigned long>(snapshot.heapPreRs485),
		static_cast<unsigned long>(snapshot.heapPostRs485));
	if (written < 0 || static_cast<size_t>(written) >= outSize) {
		return false;
	}
	return true;
}

bool
buildStatusBootNetJson(const StatusBootNetSnapshot &snapshot, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	const int written = A2M_SNPRINTF(
		out,
		outSize,
		A2M_FMT("{"
		        "\"wifi_connect_ms\":%lu,"
		        "\"http_started_ms\":%lu,"
		        "\"mqtt_connect_ms\":%lu,"
		        "\"wifi_begin_calls\":%lu,"
		        "\"wifi_disconnects_boot\":%lu,"
		        "\"wifi_last_disconnect_reason_boot\":%lu"
		        "}"),
		static_cast<unsigned long>(snapshot.wifiConnectMs),
		static_cast<unsigned long>(snapshot.httpStartedMs),
		static_cast<unsigned long>(snapshot.mqttConnectMs),
		static_cast<unsigned long>(snapshot.wifiBeginCalls),
		static_cast<unsigned long>(snapshot.wifiDisconnectsBoot),
		static_cast<unsigned long>(snapshot.wifiLastDisconnectReasonBoot));
	if (written < 0 || static_cast<size_t>(written) >= outSize) {
		return false;
	}
	return true;
}

bool
buildStatusPowerSnapshotDiagLastJson(const StatusPowerSnapshotDiagLastSnapshot &snapshot,
                                     char *out,
                                     size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	if (!snapshot.valid) {
		const int written = A2M_SNPRINTF(out, outSize, A2M_FMT("{\"valid\":false}"));
		return written >= 0 && static_cast<size_t>(written) < outSize;
	}
	char escapedReason[96];
	if (!appendEscapedJsonString(escapedReason, sizeof(escapedReason), snapshot.reason != nullptr ? snapshot.reason : "")) {
		return false;
	}
	out[0] = '\0';
	size_t used = 0;
	if (!appendJsonf(out,
	                 outSize,
	                 used,
	                 "{"
	                 "\"valid\":true,"
	                 "\"reason\":\"%s\","
	                 "\"ts_ms\":%lu,"
	                 "\"total_q10\":%u,"
	                 "\"load_w\":%ld,"
	                 "\"dispatch_request_queued_ms\":%lu,"
	                 "\"dispatch_last_run_ms\":%lu,",
	                 escapedReason,
	                 static_cast<unsigned long>(snapshot.tsMs),
	                 static_cast<unsigned>(snapshot.totalQ10),
	                 static_cast<long>(snapshot.loadW),
	                 static_cast<unsigned long>(snapshot.dispatchRequestQueuedMs),
	                 static_cast<unsigned long>(snapshot.dispatchLastRunMs))) {
		return false;
	}
	if (!appendPowerSnapshotDiagSubreadJson(out, outSize, used, "battery", snapshot.battery)) {
		return false;
	}
	if (!appendJsonf(out, outSize, used, ",")) {
		return false;
	}
	if (!appendPowerSnapshotDiagSubreadJson(out, outSize, used, "grid", snapshot.grid)) {
		return false;
	}
	if (!appendJsonf(out, outSize, used, ",")) {
		return false;
	}
	if (!appendPowerSnapshotDiagSubreadJson(out, outSize, used, "pv_meter", snapshot.pvMeter)) {
		return false;
	}
	if (!appendJsonf(out, outSize, used, ",")) {
		return false;
	}
	if (!appendPowerSnapshotDiagSubreadJson(out, outSize, used, "pv_block", snapshot.pvBlock)) {
		return false;
	}
	if (!appendJsonf(out, outSize, used, "}")) {
		return false;
	}
	return true;
}

bool
buildStatusPowerSnapshotDiagCountsJson(const StatusPowerSnapshotDiagCountsSnapshot &snapshot,
                                       char *out,
                                       size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	out[0] = '\0';
	size_t used = 0;
	if (!appendJsonf(out,
	                 outSize,
	                 used,
	                 "{"
	                 "\"interesting_events\":%lu,"
	                 "\"load_low_events\":%lu,",
	                 static_cast<unsigned long>(snapshot.interestingEventCount),
	                 static_cast<unsigned long>(snapshot.loadLowEventCount))) {
		return false;
	}
	if (!appendPowerSnapshotDiagCounterJson(out, outSize, used, "battery", snapshot.battery)) {
		return false;
	}
	if (!appendJsonf(out, outSize, used, ",")) {
		return false;
	}
	if (!appendPowerSnapshotDiagCounterJson(out, outSize, used, "grid", snapshot.grid)) {
		return false;
	}
	if (!appendJsonf(out, outSize, used, ",")) {
		return false;
	}
	if (!appendPowerSnapshotDiagCounterJson(out, outSize, used, "pv_meter", snapshot.pvMeter)) {
		return false;
	}
	if (!appendJsonf(out, outSize, used, ",")) {
		return false;
	}
	if (!appendPowerSnapshotDiagCounterJson(out, outSize, used, "pv_block", snapshot.pvBlock)) {
		return false;
	}
	if (!appendJsonf(out, outSize, used, "}")) {
		return false;
	}
	return true;
}
