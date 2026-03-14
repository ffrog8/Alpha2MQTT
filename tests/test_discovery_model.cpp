#include <cstring>
#include <string>

#include "doctest/doctest.h"

#include "DiscoveryModel.h"
#include "MqttEntities.h"

TEST_CASE("discovery model builds stable controller and inverter identifiers")
{
	const uint8_t mac[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x12, 0x34 };
	char controllerId[40];
	buildControllerIdentifier(mac, controllerId, sizeof(controllerId));
	CHECK(std::string(controllerId) == "alpha2mqtt_deadbeef1234");

	char inverterId[64];
	buildInverterIdentifier("AL12345678901234", inverterId, sizeof(inverterId));
	CHECK(std::string(inverterId) == "alpha2mqtt_inv_AL12345678901234");
}

TEST_CASE("discovery model rejects missing inverter serial and skips inverter topic base")
{
	CHECK_FALSE(inverterSerialIsValid(""));
	CHECK_FALSE(inverterSerialIsValid("unknown"));
	CHECK_FALSE(inverterHaUniqueIdMatchesSerial("A2M-AL12345678901234", ""));
	CHECK_FALSE(inverterHaUniqueIdMatchesSerial("A2M-UNKNOWN", "AL12345678901234"));

	char topicBase[128];
	const bool ok = buildEntityTopicBase("Alpha2MQTT-123456",
	                                     DiscoveryDeviceScope::Inverter,
	                                     "alpha2mqtt_deadbeef1234",
	                                     "",
	                                     "State_of_Charge",
	                                     topicBase,
	                                     sizeof(topicBase));
	CHECK_FALSE(ok);
	CHECK(topicBase[0] == '\0');
}

TEST_CASE("discovery model routes controller entities and inverter unique_id uses via-serial identity")
{
	CHECK(mqttEntityScope(mqttEntityId::entityA2MUptime) == DiscoveryDeviceScope::Controller);
	CHECK(mqttEntityScope(mqttEntityId::entityBatSoc) == DiscoveryDeviceScope::Inverter);
	CHECK(mqttEntityScope(mqttEntityId::entityPollingBudgetExceeded) == DiscoveryDeviceScope::Controller);

	char uidController[96];
	buildEntityUniqueId(DiscoveryDeviceScope::Controller,
	                    "alpha2mqtt_deadbeef1234",
	                    "",
	                    "A2M_uptime",
	                    uidController,
	                    sizeof(uidController));
	CHECK(std::string(uidController) == "alpha2mqtt_deadbeef1234_A2M_uptime");

	char uidInverter[128];
	buildEntityUniqueId(DiscoveryDeviceScope::Inverter,
	                    "alpha2mqtt_deadbeef1234",
	                    "AL12345678901234",
	                    "battery_soc",
	                    uidInverter,
	                    sizeof(uidInverter));
	CHECK(std::string(uidInverter) == "alpha2mqtt_inv_AL12345678901234_battery_soc");
}

TEST_CASE("discovery model builds stable controller and inverter topic bases")
{
	char topicBase[160];
	CHECK(buildEntityTopicBase("Alpha2MQTT-123456",
	                          DiscoveryDeviceScope::Controller,
	                          "alpha2mqtt_deadbeef1234",
	                          "AL12345678901234",
	                          "Polling_Budget_Exceeded",
	                          topicBase,
	                          sizeof(topicBase)));
	CHECK(std::string(topicBase) == "Alpha2MQTT-123456/alpha2mqtt_deadbeef1234/Polling_Budget_Exceeded");

	CHECK(buildEntityTopicBase("Alpha2MQTT-123456",
	                          DiscoveryDeviceScope::Inverter,
	                          "alpha2mqtt_deadbeef1234",
	                          "AL12345678901234",
	                          "State_of_Charge",
	                          topicBase,
	                          sizeof(topicBase)));
	CHECK(std::string(topicBase) == "Alpha2MQTT-123456/alpha2mqtt_inv_AL12345678901234/State_of_Charge");
}

TEST_CASE("discovery model refreshes HA identity when serial changes")
{
	char uniqueId[32];
	buildInverterHaUniqueId("AL12345678901234", uniqueId, sizeof(uniqueId));
	CHECK(std::string(uniqueId) == "A2M-AL12345678901234");
	CHECK(inverterHaUniqueIdMatchesSerial(uniqueId, "AL12345678901234"));
	CHECK_FALSE(inverterHaUniqueIdMatchesSerial(uniqueId, "AL00000000000000"));
}

TEST_CASE("discovery model returns the stale inverter namespace when serial changes")
{
	char staleId[64];
	CHECK(buildStaleInverterIdentifier("AL12345678901234",
	                                  "AL00000000000000",
	                                  staleId,
	                                  sizeof(staleId)));
	CHECK(std::string(staleId) == "alpha2mqtt_inv_AL12345678901234");
	CHECK_FALSE(buildStaleInverterIdentifier("AL12345678901234",
	                                        "AL12345678901234",
	                                        staleId,
	                                        sizeof(staleId)));
	CHECK_FALSE(buildStaleInverterIdentifier("unknown",
	                                        "AL00000000000000",
	                                        staleId,
	                                        sizeof(staleId)));
}

TEST_CASE("discovery model derives inverter labels for display and ids")
{
	char display[16];
	char id[16];
	CHECK(buildInverterLabelDisplay("STUBSN000000054", "", display, sizeof(display)));
	CHECK(std::string(display) == "054");
	buildInverterLabelId(display, id, sizeof(id));
	CHECK(std::string(id) == "054");

	CHECK(buildInverterLabelDisplay("STUBSN000000054", "Shed-A", display, sizeof(display)));
	CHECK(std::string(display) == "Shed-A");
	buildInverterLabelId(display, id, sizeof(id));
	CHECK(std::string(id) == "shed_a");

	char deviceName[32];
	CHECK(buildInverterDeviceDisplayName("STUBSN000000054", "Shed-A", deviceName, sizeof(deviceName)));
	CHECK(std::string(deviceName) == "Alpha Shed-A");
}

TEST_CASE("discovery model builds canonical inverter metric ids and display names")
{
	mqttState batterySoc{};
	REQUIRE(mqttEntityCopyById(mqttEntityId::entityBatSoc, &batterySoc));
	char metricId[64];
	char displayName[64];
	buildEntityMetricId(&batterySoc, metricId, sizeof(metricId));
	buildEntityDisplayName(&batterySoc, DiscoveryDeviceScope::Inverter, displayName, sizeof(displayName));
	CHECK(std::string(metricId) == "battery_soc");
	CHECK(std::string(displayName) == "Battery SOC");

	mqttState batteryCurrent{};
	REQUIRE(mqttEntityCopyById(mqttEntityId::entityBatteryCurrent, &batteryCurrent));
	buildEntityMetricId(&batteryCurrent, metricId, sizeof(metricId));
	buildEntityDisplayName(&batteryCurrent, DiscoveryDeviceScope::Inverter, displayName, sizeof(displayName));
	CHECK(std::string(metricId) == "battery_current");
	CHECK(std::string(displayName) == "Battery Current");

	mqttState controllerUptime{};
	REQUIRE(mqttEntityCopyById(mqttEntityId::entityA2MUptime, &controllerUptime));
	buildEntityMetricId(&controllerUptime, metricId, sizeof(metricId));
	buildEntityDisplayName(&controllerUptime, DiscoveryDeviceScope::Controller, displayName, sizeof(displayName));
	CHECK(std::string(metricId) == "a2m_uptime");
	CHECK(std::string(displayName) == "A2M uptime");
}
