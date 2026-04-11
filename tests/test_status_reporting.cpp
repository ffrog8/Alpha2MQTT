// Purpose: Validate status JSON formatting and event limiter logic in host tests.
#include <cstring>
#include <string>

#include "doctest/doctest.h"

#include "Definitions.h"
#include "StatusReporting.h"

TEST_CASE("event code names are stable")
{
	CHECK(strcmp(eventCodeName(MqttEventCode::Rs485Timeout), "RS485_TIMEOUT") == 0);
	CHECK(strcmp(eventCodeName(MqttEventCode::ModbusFrame), "MODBUS_FRAME") == 0);
	CHECK(strcmp(eventCodeName(MqttEventCode::PollOverrun), "POLL_OVERRUN") == 0);
	CHECK(strcmp(eventCodeName(MqttEventCode::WifiDisconnect), "WIFI_DISCONNECT") == 0);
}

TEST_CASE("event limiter enforces min interval per code")
{
	EventLimiter limiter;
	CHECK(limiter.shouldPublish(MqttEventCode::MqttDisconnect, 1000, 30000));
	CHECK_FALSE(limiter.shouldPublish(MqttEventCode::MqttDisconnect, 20000, 30000));
	CHECK(limiter.shouldPublish(MqttEventCode::MqttDisconnect, 40000, 30000));
}

TEST_CASE("event limiter treats zero timestamp as a real prior publish")
{
	EventLimiter limiter;
	CHECK(limiter.shouldPublish(MqttEventCode::WifiDisconnect, 0, 30000));
	CHECK_FALSE(limiter.shouldPublish(MqttEventCode::WifiDisconnect, 1000, 30000));
	CHECK(limiter.shouldPublish(MqttEventCode::WifiDisconnect, 30000, 30000));
}

TEST_CASE("status core JSON builder includes required keys")
{
	StatusCoreSnapshot snapshot{};
	snapshot.presence = "online";
	snapshot.a2mStatus = "online";
	snapshot.rs485Status = "OK";
	snapshot.gridStatus = "OK";
	snapshot.bootMode = "normal";
	snapshot.bootIntent = "normal";
	snapshot.httpControlPlaneEnabled = true;
	snapshot.haUniqueId = "A2M-TEST";

	char buffer[2048];
	CHECK(buildStatusCoreJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"presence\":\"online\"") != std::string::npos);
	CHECK(payload.find("\"a2mStatus\":\"online\"") != std::string::npos);
	CHECK(payload.find("\"rs485Status\":\"OK\"") != std::string::npos);
	CHECK(payload.find("\"gridStatus\":\"OK\"") != std::string::npos);
	CHECK(payload.find("\"boot_mode\":\"normal\"") != std::string::npos);
	CHECK(payload.find("\"boot_intent\":\"normal\"") != std::string::npos);
	CHECK(payload.find("\"http_control_plane_enabled\":true") != std::string::npos);
	CHECK(payload.find("\"ha_unique_id\":\"A2M-TEST\"") != std::string::npos);
}

TEST_CASE("status core JSON builder allows masked inverter identity before live serial")
{
	StatusCoreSnapshot snapshot{};
	snapshot.presence = "online";
	snapshot.a2mStatus = "online";
	snapshot.rs485Status = "unknown";
	snapshot.gridStatus = "unknown";
	snapshot.bootMode = "normal";
	snapshot.bootIntent = "normal";
	snapshot.httpControlPlaneEnabled = true;
	snapshot.haUniqueId = "A2M-UNKNOWN";

	char buffer[4096];
	CHECK(buildStatusCoreJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"ha_unique_id\":\"A2M-UNKNOWN\"") != std::string::npos);
	CHECK(payload.find("\"rs485Status\":\"unknown\"") != std::string::npos);
	CHECK(payload.find("\"gridStatus\":\"unknown\"") != std::string::npos);
}

TEST_CASE("status net JSON builder includes required keys")
{
	StatusNetSnapshot snapshot{};
	snapshot.uptimeS = 123;
	snapshot.freeHeap = 45678;
	snapshot.maxBlock = 32000;
	snapshot.fragPct = 14;
	snapshot.minFreeHeap = 4096;
	snapshot.minMaxBlock = 2048;
	snapshot.maxFragPct = 35;
	snapshot.rssiDbm = -55;
	snapshot.ip = "192.168.1.50";
	snapshot.ssid = "testwifi";
	snapshot.mqttConnected = true;
	snapshot.mqttReconnects = 2;
	snapshot.wifiStatus = "Connected";
	snapshot.wifiStatusCode = 3;
	snapshot.wifiReconnects = 1;

	char buffer[4096];
	CHECK(buildStatusNetJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"uptime_s\":123") != std::string::npos);
	CHECK(payload.find("\"max_block\":32000") != std::string::npos);
	CHECK(payload.find("\"frag_pct\":14") != std::string::npos);
	CHECK(payload.find("\"min_free_heap\":4096") != std::string::npos);
	CHECK(payload.find("\"min_max_block\":2048") != std::string::npos);
	CHECK(payload.find("\"max_frag_pct\":35") != std::string::npos);
	CHECK(payload.find("\"mqtt_reconnects\":2") != std::string::npos);
	CHECK(payload.find("\"wifi_status\":\"Connected\"") != std::string::npos);
}

TEST_CASE("status net JSON builder escapes SSID content")
{
	StatusNetSnapshot snapshot{};
	snapshot.uptimeS = 1;
	snapshot.freeHeap = 2;
	snapshot.rssiDbm = -40;
	snapshot.ip = "192.168.1.50";
	snapshot.ssid = "qa\\\"wifi";
	snapshot.mqttConnected = true;
	snapshot.mqttReconnects = 0;
	snapshot.wifiStatus = "Connected";
	snapshot.wifiStatusCode = 3;
	snapshot.wifiReconnects = 0;

	char buffer[1024];
	CHECK(buildStatusNetJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"ssid\":\"qa\\\\\\\"wifi\"") != std::string::npos);
}

TEST_CASE("status poll JSON builder includes required keys")
{
	StatusPollSnapshot snapshot{};
	snapshot.pollOkCount = 10;
	snapshot.heapFreeB = 5555;
	snapshot.heapMaxBlockB = 4444;
	snapshot.heapFragPct = 12;
	snapshot.memLevel = 1;
	snapshot.bootHeapLevel = 2;
	snapshot.bootHeapStage = 3;
	snapshot.bootHeapFreeB = 3333;
	snapshot.bootHeapMaxBlockB = 2222;
	snapshot.bootHeapFragPct = 34;
	snapshot.pollErrCount = 1;
	snapshot.rs485ErrorCount = 9;
	snapshot.rs485TransportErrorCount = 4;
	snapshot.rs485OtherErrorCount = 5;
	snapshot.lastPollMs = 250;
	snapshot.lastOkTsMs = 10000;
	snapshot.lastErrTsMs = 11000;
	snapshot.lastErrCode = 2;
	snapshot.rs485ProbeLastAttemptMs = 12345;
	snapshot.rs485ProbeBackoffMs = 15000;
	snapshot.rs485ConnectionEpoch = 6;
	snapshot.rs485BaudConfigured = 115200;
	snapshot.rs485BaudActual = 9600;
	snapshot.rs485BaudSync = "mismatch";
	snapshot.rs485Backend = "stub";
	snapshot.inverterReady = true;
	snapshot.essSnapshotOk = false;
	snapshot.essSnapshotLastOk = false;
	snapshot.essSnapshotAttempts = 3;
	snapshot.essPowerSnapshotLastBuildMs = 91;
	snapshot.snapshotPublishSkipCount = 7;
	snapshot.rs485StubMode = "offline";
	snapshot.rs485StubFailRemaining = 0;
	snapshot.rs485StubWriteCount = 3;
	snapshot.rs485StubLastWriteStartReg = 4123;
	snapshot.rs485StubLastWriteRegCount = 9;
	snapshot.rs485StubLastWriteMs = 4242;
	snapshot.dispatchRequestQueuedMs = 4000;
	snapshot.dispatchLastRunMs = 0;
	snapshot.dispatchWaitDueToSnapshotMs = 33;
	snapshot.dispatchQueueCoalesceCount = 4;
	snapshot.dispatchBlockCacheHitCount = 5;
	snapshot.pvBlockCacheHitCount = 6;
	snapshot.pvMeterCacheHitCount = 7;
	snapshot.dispatchLastSkipReason = "ess_snapshot_failed";
	snapshot.worstPhase = "bucket_publish";
	snapshot.worstFreeHeapB = 2048;
	snapshot.worstMaxBlockB = 1024;
	snapshot.worstFragPct = 41;
	snapshot.mqttMaxPayloadSeen = 612;
	snapshot.mqttMaxPayloadKind = "entity";
	snapshot.pollIntervalSeconds = 30;
	snapshot.schedTenSecLastRunMs = 0;
	snapshot.schedOneMinLastRunMs = 0;
	snapshot.schedFiveMinLastRunMs = 0;
	snapshot.schedOneHourLastRunMs = 0;
	snapshot.schedOneDayLastRunMs = 0;
	snapshot.schedUserLastRunMs = 0;
	snapshot.schedTenSecCount = 3;
	snapshot.schedOneMinCount = 4;
	snapshot.schedFiveMinCount = 5;
	snapshot.schedOneHourCount = 6;
	snapshot.schedOneDayCount = 7;
	snapshot.schedUserCount = 8;
	snapshot.persistLoadOk = 1;
	snapshot.persistLoadErr = 0;
	snapshot.persistUnknownEntityCount = 2;
	snapshot.persistInvalidBucketCount = 3;
	snapshot.persistDuplicateEntityCount = 4;
	snapshot.pollingBudgetExceeded = true;
	snapshot.pollingBudgetOverrunCount = 5;
	snapshot.pollingBudgetUsedMs[0] = 123;
	snapshot.pollingBudgetLimitMs[0] = 5000;
	snapshot.pollingBacklogCount[0] = 2;
	snapshot.pollingBacklogOldestAgeMs[0] = 7000;
	snapshot.pollingLastFullCycleAgeMs[0] = 11000;

	char buffer[2048];
	CHECK(buildStatusPollJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"rs485_backend\":\"stub\"") != std::string::npos);
	CHECK(payload.find("\"rs485_stub_mode\":\"offline\"") != std::string::npos);
	CHECK(payload.find("\"rs485_stub_fail_remaining\":0") != std::string::npos);
	CHECK(payload.find("\"rs485_stub_writes\":3") != std::string::npos);
	CHECK(payload.find("\"rs485_stub_last_write_reg\":4123") != std::string::npos);
	CHECK(payload.find("\"rs485_stub_last_write_reg_count\":9") != std::string::npos);
	CHECK(payload.find("\"rs485_stub_last_write_ms\":4242") != std::string::npos);
	CHECK(payload.find("\"dispatch_request_queued_ms\":4000") != std::string::npos);
	CHECK(payload.find("\"inverter_ready\":true") != std::string::npos);
	CHECK(payload.find("\"ess_snapshot_ok\":false") != std::string::npos);
	CHECK(payload.find("\"rs485_baud_configured\":115200") != std::string::npos);
	CHECK(payload.find("\"rs485_baud_actual\":9600") != std::string::npos);
	CHECK(payload.find("\"rs485_baud_sync\":\"mismatch\"") != std::string::npos);
	CHECK(payload.find("\"mem\":{\"f\":5555") != std::string::npos);
	CHECK(payload.find("\"mem\":{\"f\":5555,\"m\":4444,\"g\":12,\"l\":1}") != std::string::npos);
	CHECK(payload.find("\"boot_mem\":{\"l\":2,\"s\":3,\"f\":3333,\"m\":2222,\"g\":34}") != std::string::npos);
	CHECK(payload.find("\"ess_snapshot_last_ok\":false") != std::string::npos);
	CHECK(payload.find("\"ess_snapshot_attempts\":3") != std::string::npos);
	CHECK(payload.find("\"ess_power_snapshot_last_build_ms\":91") != std::string::npos);
	CHECK(payload.find("\"snapshot_publish_skip_count\":7") != std::string::npos);
	CHECK(payload.find("\"dispatch_last_run_ms\":0") != std::string::npos);
	CHECK(payload.find("\"dispatch_wait_due_to_snapshot_ms\":33") != std::string::npos);
	CHECK(payload.find("\"dispatch_queue_coalesce_count\":4") != std::string::npos);
	CHECK(payload.find("\"dispatch_block_cache_hit_count\":5") != std::string::npos);
	CHECK(payload.find("\"pv_block_cache_hit_count\":6") != std::string::npos);
	CHECK(payload.find("\"pv_meter_cache_hit_count\":7") != std::string::npos);
	CHECK(payload.find("\"dispatch_last_skip_reason\":\"ess_snapshot_failed\"") != std::string::npos);
	CHECK(payload.find("\"worst_phase\":\"bucket_publish\"") != std::string::npos);
	CHECK(payload.find("\"worst_free_heap\":2048") != std::string::npos);
	CHECK(payload.find("\"worst_max_block\":1024") != std::string::npos);
	CHECK(payload.find("\"worst_frag_pct\":41") != std::string::npos);
	CHECK(payload.find("\"mqtt_max_payload_seen\":612") != std::string::npos);
	CHECK(payload.find("\"mqtt_max_payload_kind\":\"entity\"") != std::string::npos);
	CHECK(payload.find("\"poll_interval_s\":30") != std::string::npos);
	CHECK(payload.find("\"sched_user_last_run_ms\":0") != std::string::npos);
	CHECK(payload.find("\"sched_10s_count\":3") != std::string::npos);
	CHECK(payload.find("\"sched_1m_count\":4") != std::string::npos);
	CHECK(payload.find("\"sched_5m_count\":5") != std::string::npos);
	CHECK(payload.find("\"sched_1h_count\":6") != std::string::npos);
	CHECK(payload.find("\"sched_1d_count\":7") != std::string::npos);
	CHECK(payload.find("\"sched_user_count\":8") != std::string::npos);
	CHECK(payload.find("\"persist_load_ok\":1") != std::string::npos);
	CHECK(payload.find("\"persist_load_err\":0") != std::string::npos);
	CHECK(payload.find("\"persist_unknown_entity_count\":2") != std::string::npos);
	CHECK(payload.find("\"persist_invalid_bucket_count\":3") != std::string::npos);
	CHECK(payload.find("\"persist_duplicate_entity_count\":4") != std::string::npos);
	CHECK(payload.find("\"poll_budget\":{\"k\":[\"10s\",\"1m\",\"5m\",\"1h\",\"1d\",\"usr\"],\"x\":true,\"c\":5") != std::string::npos);
	CHECK(payload.find("\"u\":[123,0,0,0,0,0]") != std::string::npos);
	CHECK(payload.find("\"b\":[2,0,0,0,0,0]") != std::string::npos);
	CHECK(payload.find("\"poll_err_count\":1") != std::string::npos);
	CHECK(payload.find("\"rs485_error_count\":9") != std::string::npos);
	CHECK(payload.find("\"rs485_transport_error_count\":4") != std::string::npos);
	CHECK(payload.find("\"rs485_other_error_count\":5") != std::string::npos);
	CHECK(payload.find("\"last_err_code\":2") != std::string::npos);
	CHECK(payload.find("\"rs485_probe_last_attempt_ms\":12345") != std::string::npos);
	CHECK(payload.find("\"rs485_probe_backoff_ms\":15000") != std::string::npos);
	CHECK(payload.find("\"rs485_connection_epoch\":6") != std::string::npos);
}

TEST_CASE("status poll JSON builder escapes string fields")
{
	StatusPollSnapshot snapshot{};
	snapshot.rs485Backend = "st\\\"ub";
	snapshot.rs485StubMode = "on\\line";
	snapshot.inverterReady = true;
	snapshot.essSnapshotOk = true;
	snapshot.essSnapshotLastOk = true;
	snapshot.dispatchLastSkipReason = "bad\\\"skip";

	char buffer[2048];
	CHECK(buildStatusPollJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"rs485_backend\":\"st\\\\\\\"ub\"") != std::string::npos);
	CHECK(payload.find("\"dispatch_last_skip_reason\":\"bad\\\\\\\"skip\"") != std::string::npos);
}

TEST_CASE("status poll JSON builder keeps stub dispatch timing key names stable")
{
	StatusPollSnapshot snapshot{};
	snapshot.rs485Backend = "stub";
	snapshot.rs485StubMode = "online";
	snapshot.dispatchLastSkipReason = "";
	snapshot.rs485StubLastWriteStartReg = 4096;
	snapshot.rs485StubLastWriteRegCount = 9;
	snapshot.rs485StubLastWriteMs = 111;
	snapshot.dispatchRequestQueuedMs = 80;

	char buffer[4096];
	CHECK(buildStatusPollJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"rs485_stub_last_write_reg\":4096") != std::string::npos);
	CHECK(payload.find("\"rs485_stub_last_write_reg_count\":9") != std::string::npos);
	CHECK(payload.find("\"rs485_stub_last_write_ms\":111") != std::string::npos);
	CHECK(payload.find("\"dispatch_request_queued_ms\":80") != std::string::npos);
}

TEST_CASE("status poll compact JSON includes snapshot/dispatch and stub scheduler keys conditionally")
{
	StatusPollSnapshot snapshot{};
	snapshot.rs485Backend = "stub";
	snapshot.rs485StubMode = "online";
	snapshot.rs485StubFailRemaining = 0;
	snapshot.rs485StubWriteCount = 2;
	snapshot.rs485StubLastWriteStartReg = 4096;
	snapshot.rs485StubLastWriteRegCount = 9;
	snapshot.rs485StubLastWriteMs = 111;
	snapshot.dispatchRequestQueuedMs = 80;
	snapshot.inverterReady = true;
	snapshot.essSnapshotOk = true;
	snapshot.essSnapshotLastOk = true;
	snapshot.essSnapshotAttempts = 42;
	snapshot.essPowerSnapshotLastBuildMs = 88;
	snapshot.snapshotPublishSkipCount = 9;
	snapshot.dispatchLastRunMs = 1000;
	snapshot.dispatchWaitDueToSnapshotMs = 44;
	snapshot.dispatchQueueCoalesceCount = 2;
	snapshot.dispatchBlockCacheHitCount = 11;
	snapshot.pvBlockCacheHitCount = 12;
	snapshot.pvMeterCacheHitCount = 13;
	snapshot.dispatchLastSkipReason = "";
	snapshot.worstPhase = "dispatch_force_publish";
	snapshot.worstFreeHeapB = 2222;
	snapshot.worstMaxBlockB = 1111;
	snapshot.worstFragPct = 37;
	snapshot.mqttMaxPayloadSeen = 480;
	snapshot.mqttMaxPayloadKind = "poll";
	snapshot.pollIntervalSeconds = 30;
	snapshot.schedTenSecLastRunMs = 10;
	snapshot.schedOneMinLastRunMs = 60;
	snapshot.schedFiveMinLastRunMs = 300;
	snapshot.schedOneHourLastRunMs = 3600;
	snapshot.schedOneDayLastRunMs = 86400;
	snapshot.schedUserLastRunMs = 123;
	snapshot.lastPollMs = 250;
	snapshot.pollOkCount = 9;
	snapshot.pollErrCount = 1;
	snapshot.rs485ErrorCount = 12;
	snapshot.rs485TransportErrorCount = 7;
	snapshot.rs485OtherErrorCount = 5;
	snapshot.rs485ProbeLastAttemptMs = 5000;
	snapshot.rs485ProbeBackoffMs = 15000;
	snapshot.rs485ConnectionEpoch = 9;
	snapshot.rs485BaudConfigured = 19200;
	snapshot.rs485BaudActual = 19200;
	snapshot.rs485BaudSync = "synced";
	snapshot.pollingBudgetExceeded = false;
	snapshot.pollingBudgetOverrunCount = 3;
	snapshot.pollingBudgetUsedMs[1] = 321;
	snapshot.pollingBudgetLimitMs[1] = 5000;
	snapshot.pollingBacklogCount[1] = 1;
	snapshot.pollingBacklogOldestAgeMs[1] = 8000;
	snapshot.pollingLastFullCycleAgeMs[1] = 12000;

	char buffer[1536];
	CHECK(buildStatusPollJsonCompact(snapshot, buffer, sizeof(buffer)));
	char mqttSizedBuffer[MAX_MQTT_PAYLOAD_SIZE];
	CHECK(buildStatusPollJsonCompact(snapshot, mqttSizedBuffer, sizeof(mqttSizedBuffer)));
	CHECK(std::strlen(mqttSizedBuffer) < sizeof(mqttSizedBuffer));
	std::string payload(buffer);

	CHECK(payload.find("\"inverter_ready\":true") != std::string::npos);
	CHECK(payload.find("\"ess_snapshot_ok\":true") != std::string::npos);
	CHECK(payload.find("\"ess_snapshot_attempts\":42") != std::string::npos);
	CHECK(payload.find("\"dispatch_last_run_ms\":1000") != std::string::npos);
	CHECK(payload.find("\"worst_phase\":\"dispatch_force_publish\"") != std::string::npos);
	CHECK(payload.find("\"mqtt_max_payload_seen\":480") != std::string::npos);
	CHECK(payload.find("\"mqtt_max_payload_kind\":\"poll\"") != std::string::npos);
	CHECK(payload.find("\"rs485_stub_last_write_reg_count\":9") != std::string::npos);
	CHECK(payload.find("\"dispatch_request_queued_ms\":80") != std::string::npos);
	CHECK(payload.find("\"poll_budget\":{\"x\":false,\"c\":3") != std::string::npos);
	CHECK(payload.find("\"b\":[0,1,0,0,0,0]") != std::string::npos);
	CHECK(payload.find("\"a\":[0,8000,0,0,0,0]") != std::string::npos);
	CHECK(payload.find("\"rs485_error_count\":12") != std::string::npos);
	CHECK(payload.find("\"rs485_transport_error_count\":7") != std::string::npos);
	CHECK(payload.find("\"rs485_other_error_count\":5") != std::string::npos);
	CHECK(payload.find("\"rs485_connection_epoch\":9") != std::string::npos);
	CHECK(payload.find("\"rs485_baud_configured\":19200") != std::string::npos);
	CHECK(payload.find("\"rs485_baud_actual\":19200") != std::string::npos);
	CHECK(payload.find("\"rs485_baud_sync\":\"synced\"") != std::string::npos);

#if RS485_STUB
	CHECK(payload.find("\"s10_ms\":10") != std::string::npos);
	CHECK(payload.find("\"s60_ms\":60") != std::string::npos);
	CHECK(payload.find("\"s300_ms\":300") != std::string::npos);
	CHECK(payload.find("\"s3600_ms\":3600") != std::string::npos);
	CHECK(payload.find("\"s86400_ms\":86400") != std::string::npos);
	CHECK(payload.find("\"su_ms\":123") != std::string::npos);
#else
	CHECK(payload.find("\"s10_ms\":") == std::string::npos);
	CHECK(payload.find("\"s60_ms\":") == std::string::npos);
	CHECK(payload.find("\"s300_ms\":") == std::string::npos);
	CHECK(payload.find("\"s3600_ms\":") == std::string::npos);
	CHECK(payload.find("\"s86400_ms\":") == std::string::npos);
	CHECK(payload.find("\"su_ms\":") == std::string::npos);
#endif
}

TEST_CASE("status publishes liveness when inverter not ready and snapshot invalid")
{
	StatusCoreSnapshot core{};
	core.presence = "online";
	core.a2mStatus = "online";
	core.rs485Status = "unknown";
	core.gridStatus = "unknown";
	core.bootMode = "normal";
	core.bootIntent = "normal";
	core.httpControlPlaneEnabled = true;
	core.haUniqueId = "A2M-TEST";

	StatusPollSnapshot poll{};
	poll.rs485Backend = "real";
	poll.inverterReady = false;
	poll.essSnapshotOk = false;
	poll.essSnapshotLastOk = false;
	poll.pollIntervalSeconds = 30;
	poll.pollOkCount = 7;
	poll.pollErrCount = 1;
	poll.rs485ProbeLastAttemptMs = 2500;
	poll.rs485ProbeBackoffMs = 15000;
	poll.rs485ConnectionEpoch = 2;
	poll.pollingBudgetExceeded = true;
	poll.pollingBudgetOverrunCount = 2;

	char coreBuffer[256];
	char pollBuffer[1536];
	CHECK(buildStatusCoreJson(core, coreBuffer, sizeof(coreBuffer)));
	CHECK(buildStatusPollJsonCompact(poll, pollBuffer, sizeof(pollBuffer)));

	std::string corePayload(coreBuffer);
	std::string pollPayload(pollBuffer);
	CHECK(corePayload.find("\"rs485Status\":\"unknown\"") != std::string::npos);
	CHECK(corePayload.find("\"gridStatus\":\"unknown\"") != std::string::npos);
	CHECK(pollPayload.find("\"inverter_ready\":false") != std::string::npos);
	CHECK(pollPayload.find("\"ess_snapshot_ok\":false") != std::string::npos);
	CHECK(pollPayload.find("\"rs485_backend\":\"real\"") != std::string::npos);
	CHECK(pollPayload.find("\"poll_ok_count\":7") != std::string::npos);
	CHECK(pollPayload.find("\"poll_err_count\":1") != std::string::npos);
	CHECK(pollPayload.find("\"poll_budget\":{\"x\":true,\"c\":2") != std::string::npos);
	CHECK(pollPayload.find("\"rs485_connection_epoch\":2") != std::string::npos);
}

TEST_CASE("status publishes liveness when inverter ready but snapshot failed")
{
	StatusCoreSnapshot core{};
	core.presence = "online";
	core.a2mStatus = "online";
	core.rs485Status = "unknown";
	core.gridStatus = "unknown";
	core.bootMode = "normal";
	core.bootIntent = "normal";
	core.httpControlPlaneEnabled = true;
	core.haUniqueId = "A2M-TEST";

	StatusPollSnapshot poll{};
	poll.rs485Backend = "real";
	poll.inverterReady = true;
	poll.essSnapshotOk = false;
	poll.essSnapshotLastOk = false;
	poll.essSnapshotAttempts = 12;
	poll.dispatchLastRunMs = 0;
	poll.pollIntervalSeconds = 30;
	poll.pollOkCount = 2;
	poll.pollErrCount = 5;
	poll.rs485ConnectionEpoch = 4;

	char coreBuffer[256];
	char pollBuffer[1536];
	CHECK(buildStatusCoreJson(core, coreBuffer, sizeof(coreBuffer)));
	CHECK(buildStatusPollJsonCompact(poll, pollBuffer, sizeof(pollBuffer)));

	std::string corePayload(coreBuffer);
	std::string pollPayload(pollBuffer);
	CHECK(corePayload.find("\"rs485Status\":\"unknown\"") != std::string::npos);
	CHECK(corePayload.find("\"gridStatus\":\"unknown\"") != std::string::npos);
	CHECK(pollPayload.find("\"inverter_ready\":true") != std::string::npos);
	CHECK(pollPayload.find("\"ess_snapshot_ok\":false") != std::string::npos);
	CHECK(pollPayload.find("\"ess_snapshot_attempts\":12") != std::string::npos);
	CHECK(pollPayload.find("\"rs485_connection_epoch\":4") != std::string::npos);
}

TEST_CASE("status includes ess-derived fields when snapshot ok")
{
	StatusCoreSnapshot core{};
	core.presence = "online";
	core.a2mStatus = "online";
	core.rs485Status = "OK";
	core.gridStatus = "OK";
	core.bootMode = "normal";
	core.bootIntent = "normal";
	core.httpControlPlaneEnabled = true;
	core.haUniqueId = "A2M-TEST";

	StatusPollSnapshot poll{};
	poll.rs485Backend = "real";
	poll.inverterReady = true;
	poll.essSnapshotOk = true;
	poll.essSnapshotLastOk = true;
	poll.essSnapshotAttempts = 44;
	poll.dispatchLastRunMs = 1000;
	poll.pollIntervalSeconds = 30;
	poll.pollOkCount = 11;
	poll.pollErrCount = 0;
	poll.rs485ConnectionEpoch = 5;

	char coreBuffer[256];
	char pollBuffer[1536];
	CHECK(buildStatusCoreJson(core, coreBuffer, sizeof(coreBuffer)));
	CHECK(buildStatusPollJsonCompact(poll, pollBuffer, sizeof(pollBuffer)));

	std::string corePayload(coreBuffer);
	std::string pollPayload(pollBuffer);
	CHECK(corePayload.find("\"rs485Status\":\"OK\"") != std::string::npos);
	CHECK(corePayload.find("\"gridStatus\":\"OK\"") != std::string::npos);
	CHECK(pollPayload.find("\"inverter_ready\":true") != std::string::npos);
	CHECK(pollPayload.find("\"ess_snapshot_ok\":true") != std::string::npos);
	CHECK(pollPayload.find("\"ess_snapshot_attempts\":44") != std::string::npos);
	CHECK(pollPayload.find("\"rs485_connection_epoch\":5") != std::string::npos);
}

TEST_CASE("status stub JSON builder includes required keys")
{
	StatusStubSnapshot snapshot{};
	snapshot.stubReads = 12;
	snapshot.stubWrites = 3;
	snapshot.stubUnknownReads = 4;
	snapshot.socX10 = 655;
	snapshot.lastReadStartReg = 123;
	snapshot.lastReadRegCount = 24;
	snapshot.lastFn = 3;
	snapshot.lastFailStartReg = 456;
	snapshot.lastFailFn = 16;
	snapshot.lastFailType = "no_response";
	snapshot.lastWriteFailStartReg = 2176;
	snapshot.lastWriteFailFn = 16;
	snapshot.lastWriteFailType = "slave_error";
	snapshot.failRegister = 2176;
	snapshot.failType = "slave_error";
	snapshot.latencyMs = 150;
	snapshot.strictUnknown = true;
	snapshot.failReads = false;
	snapshot.failWrites = true;
	snapshot.failEveryN = 2;
	snapshot.failForMs = 5000;
	snapshot.flapOnlineMs = 30000;
	snapshot.flapOfflineMs = 10000;
	snapshot.probeAttempts = 7;
	snapshot.probeSuccessAfterN = 9;
	snapshot.socStepX10PerSnapshot = -5;

	char buffer[768];
	CHECK(buildStatusStubJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"stub_reads\":12") != std::string::npos);
	CHECK(payload.find("\"stub_writes\":3") != std::string::npos);
	CHECK(payload.find("\"stub_unknown_reads\":4") != std::string::npos);
	CHECK(payload.find("\"soc_x10\":655") != std::string::npos);
	CHECK(payload.find("\"last_read_reg\":123") != std::string::npos);
	CHECK(payload.find("\"last_read_reg_count\":24") != std::string::npos);
	CHECK(payload.find("\"last_fn\":3") != std::string::npos);
	CHECK(payload.find("\"last_fail_reg\":456") != std::string::npos);
	CHECK(payload.find("\"last_fail_fn\":16") != std::string::npos);
	CHECK(payload.find("\"last_fail_type\":\"no_response\"") != std::string::npos);
	CHECK(payload.find("\"last_write_fail_reg\":2176") != std::string::npos);
	CHECK(payload.find("\"last_write_fail_fn\":16") != std::string::npos);
	CHECK(payload.find("\"last_write_fail_type\":\"slave_error\"") != std::string::npos);
	CHECK(payload.find("\"fail_reg\":2176") != std::string::npos);
	CHECK(payload.find("\"fail_type\":\"slave_error\"") != std::string::npos);
	CHECK(payload.find("\"latency_ms\":150") != std::string::npos);
	CHECK(payload.find("\"strict_unknown\":true") != std::string::npos);
	CHECK(payload.find("\"fail_reads\":false") != std::string::npos);
	CHECK(payload.find("\"fail_writes\":true") != std::string::npos);
	CHECK(payload.find("\"fail_every_n\":2") != std::string::npos);
	CHECK(payload.find("\"fail_for_ms\":5000") != std::string::npos);
	CHECK(payload.find("\"flap_online_ms\":30000") != std::string::npos);
	CHECK(payload.find("\"flap_offline_ms\":10000") != std::string::npos);
	CHECK(payload.find("\"probe_attempts\":7") != std::string::npos);
	CHECK(payload.find("\"probe_success_after_n\":9") != std::string::npos);
	CHECK(payload.find("\"soc_step_x10_per_snapshot\":-5") != std::string::npos);
}

TEST_CASE("status stub JSON builder needs the firmware-sized buffer for worst-case payloads")
{
	StatusStubSnapshot snapshot{};
	snapshot.stubReads = 4294967295UL;
	snapshot.stubWrites = 4294967295UL;
	snapshot.stubUnknownReads = 4294967295UL;
	snapshot.socX10 = 65535;
	snapshot.lastReadStartReg = 65535;
	snapshot.lastReadRegCount = 65535;
	snapshot.lastFn = 255;
	snapshot.lastFailStartReg = 65535;
	snapshot.lastFailFn = 255;
	snapshot.lastFailType = "strict_unknown_register_failure";
	snapshot.lastWriteFailStartReg = 65535;
	snapshot.lastWriteFailFn = 255;
	snapshot.lastWriteFailType = "strict_unknown_register_failure";
	snapshot.failRegister = 65535;
	snapshot.failType = "strict_unknown_register_failure";
	snapshot.latencyMs = 65535;
	snapshot.strictUnknown = true;
	snapshot.failReads = true;
	snapshot.failWrites = true;
	snapshot.failEveryN = 4294967295UL;
	snapshot.failForMs = 4294967295UL;
	snapshot.flapOnlineMs = 4294967295UL;
	snapshot.flapOfflineMs = 4294967295UL;
	snapshot.probeAttempts = 4294967295UL;
	snapshot.probeSuccessAfterN = 4294967295UL;
	snapshot.socStepX10PerSnapshot = 32767;

	char tooSmall[512];
	CHECK_FALSE(buildStatusStubJson(snapshot, tooSmall, sizeof(tooSmall)));

	char firmwareSized[768];
	CHECK(buildStatusStubJson(snapshot, firmwareSized, sizeof(firmwareSized)));
}

TEST_CASE("status manual read JSON builder includes deterministic correlation fields")
{
	StatusManualReadSnapshot snapshot{};
	snapshot.seq = 42;
	snapshot.tsMs = 123456;
	snapshot.requestedReg = 2176;
	snapshot.observedReg = 2176;
	snapshot.value = "Unknown (\"AL\")\\path";

	char buffer[256];
	CHECK(buildStatusManualReadJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"seq\":42") != std::string::npos);
	CHECK(payload.find("\"ts_ms\":123456") != std::string::npos);
	CHECK(payload.find("\"requested_reg\":2176") != std::string::npos);
	CHECK(payload.find("\"observed_reg\":2176") != std::string::npos);
	CHECK(payload.find("\"value\":\"Unknown (\\\"AL\\\")\\\\path\"") != std::string::npos);
}

TEST_CASE("status boot mem JSON builder includes all boot heap checkpoints")
{
	StatusBootMemSnapshot snapshot{};
	snapshot.fwBuildTsMs = 1774767242887ULL;
	snapshot.tsMs = 4567;
	snapshot.heapPreWifi = 19200;
	snapshot.heapPostWifi = 17864;
	snapshot.heapPostMqtt = 13248;
	snapshot.heapPreRs485 = 10688;
	snapshot.heapPostRs485 = 8480;

	char buffer[256];
	CHECK(buildStatusBootMemJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"fw_build_ts_ms\":1774767242887") != std::string::npos);
	CHECK(payload.find("\"ts_ms\":4567") != std::string::npos);
	CHECK(payload.find("\"heap_pre_wifi\":19200") != std::string::npos);
	CHECK(payload.find("\"heap_post_wifi\":17864") != std::string::npos);
	CHECK(payload.find("\"heap_post_mqtt\":13248") != std::string::npos);
	CHECK(payload.find("\"heap_pre_rs485\":10688") != std::string::npos);
	CHECK(payload.find("\"heap_post_rs485\":8480") != std::string::npos);

	char tooSmall[64];
	CHECK_FALSE(buildStatusBootMemJson(snapshot, tooSmall, sizeof(tooSmall)));
}

TEST_CASE("status boot net JSON builder includes boot network diagnostics")
{
	StatusBootNetSnapshot snapshot{};
	snapshot.wifiConnectMs = 42183;
	snapshot.httpStartedMs = 42190;
	snapshot.mqttConnectMs = 42740;
	snapshot.wifiBeginCalls = 2;
	snapshot.wifiDisconnectsBoot = 1;
	snapshot.wifiLastDisconnectReasonBoot = 8;

	char buffer[256];
	CHECK(buildStatusBootNetJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"wifi_connect_ms\":42183") != std::string::npos);
	CHECK(payload.find("\"http_started_ms\":42190") != std::string::npos);
	CHECK(payload.find("\"mqtt_connect_ms\":42740") != std::string::npos);
	CHECK(payload.find("\"wifi_begin_calls\":2") != std::string::npos);
	CHECK(payload.find("\"wifi_disconnects_boot\":1") != std::string::npos);
	CHECK(payload.find("\"wifi_last_disconnect_reason_boot\":8") != std::string::npos);

	char tooSmall[64];
	CHECK_FALSE(buildStatusBootNetJson(snapshot, tooSmall, sizeof(tooSmall)));
}
