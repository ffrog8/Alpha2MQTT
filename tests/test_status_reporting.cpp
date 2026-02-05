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

	char buffer[512];
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

	char buffer[512];
	CHECK(buildStatusNetJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"uptime_s\":123") != std::string::npos);
	CHECK(payload.find("\"mqtt_reconnects\":2") != std::string::npos);
	CHECK(payload.find("\"wifi_status\":\"Connected\"") != std::string::npos);
}

TEST_CASE("status poll JSON builder includes required keys")
{
	StatusPollSnapshot snapshot{};
	snapshot.pollOkCount = 10;
	snapshot.pollErrCount = 1;
	snapshot.lastPollMs = 250;
	snapshot.lastOkTsMs = 10000;
	snapshot.lastErrTsMs = 11000;
	snapshot.lastErrCode = 2;
	snapshot.rs485ProbeLastAttemptMs = 12345;
	snapshot.rs485ProbeBackoffMs = 15000;
	snapshot.rs485Backend = "stub";
	snapshot.essSnapshotLastOk = false;
	snapshot.essSnapshotAttempts = 3;
	snapshot.rs485StubMode = "offline";
	snapshot.rs485StubFailRemaining = 0;
	snapshot.rs485StubWriteCount = 3;
	snapshot.rs485StubLastWriteStartReg = 4123;
	snapshot.rs485StubLastWriteMs = 4242;
	snapshot.dispatchLastRunMs = 0;
	snapshot.dispatchLastSkipReason = "ess_snapshot_failed";

	char buffer[512];
	CHECK(buildStatusPollJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"rs485_backend\":\"stub\"") != std::string::npos);
	CHECK(payload.find("\"rs485_stub_mode\":\"offline\"") != std::string::npos);
	CHECK(payload.find("\"rs485_stub_fail_remaining\":0") != std::string::npos);
	CHECK(payload.find("\"rs485_stub_writes\":3") != std::string::npos);
	CHECK(payload.find("\"rs485_stub_last_write_reg\":4123") != std::string::npos);
	CHECK(payload.find("\"rs485_stub_last_write_ms\":4242") != std::string::npos);
	CHECK(payload.find("\"ess_snapshot_last_ok\":false") != std::string::npos);
	CHECK(payload.find("\"ess_snapshot_attempts\":3") != std::string::npos);
	CHECK(payload.find("\"dispatch_last_run_ms\":0") != std::string::npos);
	CHECK(payload.find("\"dispatch_last_skip_reason\":\"ess_snapshot_failed\"") != std::string::npos);
	CHECK(payload.find("\"poll_err_count\":1") != std::string::npos);
	CHECK(payload.find("\"last_err_code\":2") != std::string::npos);
	CHECK(payload.find("\"rs485_probe_last_attempt_ms\":12345") != std::string::npos);
	CHECK(payload.find("\"rs485_probe_backoff_ms\":15000") != std::string::npos);
}

TEST_CASE("status stub JSON builder includes required keys")
{
	StatusStubSnapshot snapshot{};
	snapshot.stubReads = 12;
	snapshot.stubWrites = 3;
	snapshot.stubUnknownReads = 4;
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
