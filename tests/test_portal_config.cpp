// Purpose: Lock down portal save/redirect rules so WiFi/MQTT persistence changes
// don't regress when portal behavior is adjusted.

#include <doctest/doctest.h>

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

