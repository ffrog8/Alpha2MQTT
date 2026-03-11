// Purpose: Validate status JSON formatting and event limiter logic in host tests.
#include <cstring>
#include <string>

#include "doctest/doctest.h"

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

	char buffer[1024];
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

TEST_CASE("status net JSON builder includes required keys")
{
	StatusNetSnapshot snapshot{};
	snapshot.uptimeS = 123;
	snapshot.freeHeap = 45678;
	snapshot.rssiDbm = -55;
	snapshot.ip = "192.168.1.50";
	snapshot.ssid = "testwifi";
	snapshot.mqttConnected = true;
	snapshot.mqttReconnects = 2;
	snapshot.wifiStatus = "Connected";
	snapshot.wifiStatusCode = 3;
	snapshot.wifiReconnects = 1;

	char buffer[1024];
	CHECK(buildStatusNetJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"uptime_s\":123") != std::string::npos);
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
	snapshot.lastPollMs = 250;
	snapshot.lastOkTsMs = 10000;
	snapshot.lastErrTsMs = 11000;
	snapshot.lastErrCode = 2;
	snapshot.rs485ProbeLastAttemptMs = 12345;
	snapshot.rs485ProbeBackoffMs = 15000;
	snapshot.rs485Backend = "stub";
	snapshot.inverterReady = true;
	snapshot.essSnapshotOk = false;
	snapshot.essSnapshotLastOk = false;
	snapshot.essSnapshotAttempts = 3;
	snapshot.rs485StubMode = "offline";
	snapshot.rs485StubFailRemaining = 0;
	snapshot.rs485StubWriteCount = 3;
	snapshot.rs485StubLastWriteStartReg = 4123;
	snapshot.rs485StubLastWriteMs = 4242;
	snapshot.dispatchLastRunMs = 0;
	snapshot.dispatchLastSkipReason = "ess_snapshot_failed";
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
	CHECK(payload.find("\"rs485_stub_last_write_ms\":4242") != std::string::npos);
	CHECK(payload.find("\"inverter_ready\":true") != std::string::npos);
	CHECK(payload.find("\"ess_snapshot_ok\":false") != std::string::npos);
	CHECK(payload.find("\"mem\":{\"f\":5555") != std::string::npos);
	CHECK(payload.find("\"mem\":{\"f\":5555,\"m\":4444,\"g\":12,\"l\":1}") != std::string::npos);
	CHECK(payload.find("\"boot_mem\":{\"l\":2,\"s\":3,\"f\":3333,\"m\":2222,\"g\":34}") != std::string::npos);
	CHECK(payload.find("\"ess_snapshot_last_ok\":false") != std::string::npos);
	CHECK(payload.find("\"ess_snapshot_attempts\":3") != std::string::npos);
	CHECK(payload.find("\"dispatch_last_run_ms\":0") != std::string::npos);
	CHECK(payload.find("\"dispatch_last_skip_reason\":\"ess_snapshot_failed\"") != std::string::npos);
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
	CHECK(payload.find("\"last_err_code\":2") != std::string::npos);
	CHECK(payload.find("\"rs485_probe_last_attempt_ms\":12345") != std::string::npos);
	CHECK(payload.find("\"rs485_probe_backoff_ms\":15000") != std::string::npos);
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

TEST_CASE("status poll compact JSON includes snapshot/dispatch and stub scheduler keys conditionally")
{
	StatusPollSnapshot snapshot{};
	snapshot.rs485Backend = "stub";
	snapshot.rs485StubMode = "online";
	snapshot.rs485StubFailRemaining = 0;
	snapshot.rs485StubWriteCount = 2;
	snapshot.rs485StubLastWriteStartReg = 4096;
	snapshot.rs485StubLastWriteMs = 111;
	snapshot.inverterReady = true;
	snapshot.essSnapshotOk = true;
	snapshot.essSnapshotLastOk = true;
	snapshot.essSnapshotAttempts = 42;
	snapshot.dispatchLastRunMs = 1000;
	snapshot.dispatchLastSkipReason = "";
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
	snapshot.rs485ProbeLastAttemptMs = 5000;
	snapshot.rs485ProbeBackoffMs = 15000;
	snapshot.pollingBudgetExceeded = false;
	snapshot.pollingBudgetOverrunCount = 3;
	snapshot.pollingBudgetUsedMs[1] = 321;
	snapshot.pollingBudgetLimitMs[1] = 5000;
	snapshot.pollingBacklogCount[1] = 1;
	snapshot.pollingBacklogOldestAgeMs[1] = 8000;
	snapshot.pollingLastFullCycleAgeMs[1] = 12000;

	char buffer[1536];
	CHECK(buildStatusPollJsonCompact(snapshot, buffer, sizeof(buffer)));
	std::string payload(buffer);

	CHECK(payload.find("\"inverter_ready\":true") != std::string::npos);
	CHECK(payload.find("\"ess_snapshot_ok\":true") != std::string::npos);
	CHECK(payload.find("\"ess_snapshot_attempts\":42") != std::string::npos);
	CHECK(payload.find("\"dispatch_last_run_ms\":1000") != std::string::npos);
	CHECK(payload.find("\"poll_budget\":{\"x\":false,\"c\":3") != std::string::npos);
	CHECK(payload.find("\"b\":[0,1,0,0,0,0]") != std::string::npos);
	CHECK(payload.find("\"a\":[0,8000,0,0,0,0]") != std::string::npos);

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
}

TEST_CASE("status stub JSON builder includes required keys")
{
	StatusStubSnapshot snapshot{};
	snapshot.stubReads = 12;
	snapshot.stubWrites = 3;
	snapshot.stubUnknownReads = 4;
	snapshot.socX10 = 655;
	snapshot.lastReadStartReg = 123;
	snapshot.lastFn = 3;
	snapshot.lastFailStartReg = 456;
	snapshot.lastFailFn = 16;
	snapshot.lastFailType = "no_response";
	snapshot.latencyMs = 150;
	snapshot.strictUnknown = true;
	snapshot.failEveryN = 2;
	snapshot.failForMs = 5000;
	snapshot.flapOnlineMs = 30000;
	snapshot.flapOfflineMs = 10000;
	snapshot.probeAttempts = 7;
	snapshot.probeSuccessAfterN = 9;
	snapshot.socStepX10PerSnapshot = -5;

	char buffer[512];
	CHECK(buildStatusStubJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"stub_reads\":12") != std::string::npos);
	CHECK(payload.find("\"stub_writes\":3") != std::string::npos);
	CHECK(payload.find("\"stub_unknown_reads\":4") != std::string::npos);
	CHECK(payload.find("\"soc_x10\":655") != std::string::npos);
	CHECK(payload.find("\"last_read_reg\":123") != std::string::npos);
	CHECK(payload.find("\"last_fn\":3") != std::string::npos);
	CHECK(payload.find("\"last_fail_reg\":456") != std::string::npos);
	CHECK(payload.find("\"last_fail_fn\":16") != std::string::npos);
	CHECK(payload.find("\"last_fail_type\":\"no_response\"") != std::string::npos);
	CHECK(payload.find("\"latency_ms\":150") != std::string::npos);
	CHECK(payload.find("\"strict_unknown\":true") != std::string::npos);
	CHECK(payload.find("\"fail_every_n\":2") != std::string::npos);
	CHECK(payload.find("\"fail_for_ms\":5000") != std::string::npos);
	CHECK(payload.find("\"flap_online_ms\":30000") != std::string::npos);
	CHECK(payload.find("\"flap_offline_ms\":10000") != std::string::npos);
	CHECK(payload.find("\"probe_attempts\":7") != std::string::npos);
	CHECK(payload.find("\"probe_success_after_n\":9") != std::string::npos);
	CHECK(payload.find("\"soc_step_x10_per_snapshot\":-5") != std::string::npos);
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
