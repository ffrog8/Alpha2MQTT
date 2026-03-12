#include <cstring>
#include <string>

#include "doctest/doctest.h"

#include "DiscoveryModel.h"

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
	                    "State_of_Charge",
	                    uidInverter,
	                    sizeof(uidInverter));
	CHECK(std::string(uidInverter) == "alpha2mqtt_inv_AL12345678901234_State_of_Charge");
}

TEST_CASE("discovery model refreshes HA identity when serial changes")
{
	char uniqueId[32];
	buildInverterHaUniqueId("AL12345678901234", uniqueId, sizeof(uniqueId));
	CHECK(std::string(uniqueId) == "A2M-AL12345678901234");
	CHECK(inverterHaUniqueIdMatchesSerial(uniqueId, "AL12345678901234"));
	CHECK_FALSE(inverterHaUniqueIdMatchesSerial(uniqueId, "AL00000000000000"));
}
