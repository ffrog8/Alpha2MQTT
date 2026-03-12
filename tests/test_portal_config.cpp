// Purpose: Lock down portal save/redirect rules so WiFi/MQTT persistence changes
// don't regress when portal behavior is adjusted.

#include <doctest/doctest.h>

#include <cstring>

#include "PortalConfig.h"

namespace {

static mqttState
makeEntity(const char *name, MqttEntityFamily family)
{
	mqttState entity{};
	entity.mqttName = name;
	entity.family = family;
	entity.scope = (family == MqttEntityFamily::Controller) ? MqttEntityScope::Controller : MqttEntityScope::Inverter;
	entity.readKind = MqttEntityReadKind::Register;
	return entity;
}

} // namespace

TEST_CASE("portal config: mqtt blank detection")
{
	CHECK(mqttServerIsBlank(nullptr));
	CHECK(mqttServerIsBlank(""));
	CHECK(!mqttServerIsBlank("mqtt.local"));
}

TEST_CASE("portal config: post-wifi action redirects only when mqtt blank")
{
	CHECK(portalPostWifiActionAfterWifiSave("") == PortalPostWifiAction::RedirectToMqttParams);
	CHECK(portalPostWifiActionAfterWifiSave("broker") == PortalPostWifiAction::Reboot);
}

TEST_CASE("portal config: menu includes update and no exit")
{
	PortalMenu menu = portalMenuDefault();
	CHECK(menu.items != nullptr);
	CHECK(menu.count > 0);

	bool hasUpdate = false;
	bool hasExit = false;
	bool hasCustom = false;
	int paramIndex = -1;
	int customIndex = -1;
	int updateIndex = -1;

	for (uint8_t i = 0; i < menu.count; i++) {
		const char *id = menu.items[i];
		CHECK(id != nullptr);
		if (strcmp(id, "update") == 0) {
			hasUpdate = true;
			updateIndex = i;
		}
		if (strcmp(id, "custom") == 0) {
			hasCustom = true;
			customIndex = i;
		}
		if (strcmp(id, "exit") == 0) {
			hasExit = true;
		}
		if (strcmp(id, "param") == 0) {
			paramIndex = i;
		}
	}

	CHECK(hasUpdate);
	CHECK(hasCustom);
	CHECK(!hasExit);
	CHECK(paramIndex >= 0);
	CHECK(customIndex > paramIndex);
	CHECK(updateIndex > paramIndex);
}

TEST_CASE("portal config: custom polling menu html points to polling page")
{
	const char *html = portalMenuPollingHtml();
	REQUIRE(html != nullptr);
	CHECK(strstr(html, "/config/polling") != nullptr);
	CHECK(strstr(html, "Polling") != nullptr);
}

TEST_CASE("portal config: reboot-to-normal html includes runtime redirect probe")
{
	const char *html = portalRebootToNormalHtml();
	REQUIRE(html != nullptr);
	CHECK(strstr(html, "Rebooting into normal runtime") != nullptr);
	CHECK(strstr(html, "fetch('/',{cache:'no-store'})") != nullptr);
	CHECK(strstr(html, "Alpha2MQTT Control") != nullptr);
}

TEST_CASE("portal config: polling family order is stable and controller diagnostics remain last")
{
	REQUIRE(portalPollingFamilyCount() == 7);
	CHECK(portalPollingFamilyAt(0) == MqttEntityFamily::Battery);
	CHECK(portalPollingFamilyAt(1) == MqttEntityFamily::Inverter);
	CHECK(portalPollingFamilyAt(2) == MqttEntityFamily::Backup);
	CHECK(portalPollingFamilyAt(3) == MqttEntityFamily::Pv);
	CHECK(portalPollingFamilyAt(4) == MqttEntityFamily::Grid);
	CHECK(portalPollingFamilyAt(5) == MqttEntityFamily::System);
	CHECK(portalPollingFamilyAt(6) == MqttEntityFamily::Controller);
	CHECK(strcmp(portalPollingFamilyKey(MqttEntityFamily::Controller), "controller") == 0);
	CHECK(strcmp(portalPollingFamilyLabel(MqttEntityFamily::Pv), "PV") == 0);
}

TEST_CASE("portal config: polling family keys round-trip")
{
	MqttEntityFamily family = MqttEntityFamily::Controller;
	CHECK(portalPollingFamilyFromKey("battery", &family));
	CHECK(family == MqttEntityFamily::Battery);
	CHECK(portalPollingFamilyFromKey("pv", &family));
	CHECK(family == MqttEntityFamily::Pv);
	CHECK_FALSE(portalPollingFamilyFromKey("unknown", &family));
	CHECK_FALSE(portalPollingFamilyFromKey(nullptr, &family));
}

TEST_CASE("portal config: family page normalizes requested page within matching entities only")
{
	const mqttState entities[] = {
		makeEntity("Battery_Voltage", MqttEntityFamily::Battery),
		makeEntity("Grid_Power", MqttEntityFamily::Grid),
		makeEntity("Battery_Status", MqttEntityFamily::Battery),
		makeEntity("PV1_Power", MqttEntityFamily::Pv),
		makeEntity("Battery_Temp", MqttEntityFamily::Battery),
		makeEntity("A2M_uptime", MqttEntityFamily::Controller),
	};

	PortalFamilyPage page = portalBuildFamilyPage(entities, 6, MqttEntityFamily::Battery, 5, 2);
	CHECK(page.family == MqttEntityFamily::Battery);
	CHECK(page.totalEntityCount == 3);
	CHECK(page.maxPage == 1);
	CHECK(page.safePage == 1);
	CHECK(page.pageStartOffset == 2);
	CHECK(page.pageCount == 1);
}

TEST_CASE("portal config: family page collects visible entity indices in original catalog order")
{
	const mqttState entities[] = {
		makeEntity("Battery_Voltage", MqttEntityFamily::Battery),
		makeEntity("Grid_Power", MqttEntityFamily::Grid),
		makeEntity("Battery_Status", MqttEntityFamily::Battery),
		makeEntity("PV1_Power", MqttEntityFamily::Pv),
		makeEntity("Battery_Temp", MqttEntityFamily::Battery),
		makeEntity("A2M_uptime", MqttEntityFamily::Controller),
		makeEntity("Battery_Number", MqttEntityFamily::Battery),
	};

	PortalFamilyPage page = portalBuildFamilyPage(entities, 7, MqttEntityFamily::Battery, 1, 2);
	uint16_t indices[2] = { UINT16_MAX, UINT16_MAX };
	const size_t collected = portalCollectFamilyPageEntityIndices(entities, 7, page, indices, 2);

	CHECK(page.totalEntityCount == 4);
	CHECK(page.safePage == 1);
	CHECK(page.pageCount == 2);
	CHECK(collected == 2);
	CHECK(indices[0] == 4);
	CHECK(indices[1] == 6);
}

TEST_CASE("portal config: empty families produce empty pages")
{
	const mqttState entities[] = {
		makeEntity("Grid_Power", MqttEntityFamily::Grid),
		makeEntity("PV1_Power", MqttEntityFamily::Pv),
	};

	PortalFamilyPage page = portalBuildFamilyPage(entities, 2, MqttEntityFamily::Battery, 0, 8);
	uint16_t indices[1] = { UINT16_MAX };
	CHECK(page.totalEntityCount == 0);
	CHECK(page.pageCount == 0);
	CHECK(portalCollectFamilyPageEntityIndices(entities, 2, page, indices, 1) == 0);
}
