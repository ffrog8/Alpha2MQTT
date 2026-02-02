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

	char buffer[512];
	CHECK(buildStatusPollJson(snapshot, buffer, sizeof(buffer)));

	std::string payload(buffer);
	CHECK(payload.find("\"poll_err_count\":1") != std::string::npos);
	CHECK(payload.find("\"last_err_code\":2") != std::string::npos);
	CHECK(payload.find("\"rs485_probe_last_attempt_ms\":12345") != std::string::npos);
	CHECK(payload.find("\"rs485_probe_backoff_ms\":15000") != std::string::npos);
}
