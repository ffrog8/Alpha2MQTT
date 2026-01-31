// Purpose: Lock down portal save/redirect rules so WiFi/MQTT persistence changes
// don't regress when portal behavior is adjusted.

#include <doctest/doctest.h>

#include <cstring>

#include "PortalConfig.h"

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
	int paramIndex = -1;
	int updateIndex = -1;

	for (uint8_t i = 0; i < menu.count; i++) {
		const char *id = menu.items[i];
		CHECK(id != nullptr);
		if (strcmp(id, "update") == 0) {
			hasUpdate = true;
			updateIndex = i;
		}
		if (strcmp(id, "exit") == 0) {
			hasExit = true;
		}
		if (strcmp(id, "param") == 0) {
			paramIndex = i;
		}
	}

	CHECK(hasUpdate);
	CHECK(!hasExit);
	CHECK(paramIndex >= 0);
	CHECK(updateIndex > paramIndex);
}
