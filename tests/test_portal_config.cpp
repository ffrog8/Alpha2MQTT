// Purpose: Lock down portal save/redirect rules so WiFi/MQTT persistence changes
// don't regress when portal behavior is adjusted.

#include <doctest/doctest.h>

#include <cstring>
#include <string>

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

TEST_CASE("portal config: requested family falls back to first non-empty family")
{
	const mqttState entities[] = {
		makeEntity("PV1_Power", MqttEntityFamily::Pv),
		makeEntity("A2M_uptime", MqttEntityFamily::Controller),
	};

	CHECK(portalNormalizePollingFamily(entities, 2, "pv") == MqttEntityFamily::Pv);
	CHECK(portalNormalizePollingFamily(entities, 2, "battery") == MqttEntityFamily::Pv);
	CHECK(portalNormalizePollingFamily(entities, 2, nullptr) == MqttEntityFamily::Pv);
}

TEST_CASE("portal config: polling estimate collapses snapshot and shared register reads")
{
	mqttState entities[] = {
		makeEntity("Battery_Temp", MqttEntityFamily::Battery),
		makeEntity("Max_Cell_Temp", MqttEntityFamily::Battery),
		makeEntity("Op_Mode", MqttEntityFamily::Inverter),
	};
	entities[0].needsEssSnapshot = true;
	entities[1].needsEssSnapshot = true;
	entities[2].readKind = MqttEntityReadKind::Control;
	const BucketId buckets[] = { BucketId::TenSec, BucketId::TenSec, BucketId::TenSec };

	PortalPollingEstimate estimate = portalBuildPollingEstimate(entities, 3, buckets, BucketId::TenSec, 13000, 5000);
	CHECK(estimate.entityCount == 3);
	CHECK(estimate.transactionCount == 2);
	CHECK(estimate.estimatedUsedMs == 500);
	CHECK(estimate.budgetMs == 5000);
	CHECK(estimate.level == PortalEstimateLevel::Light);
}

TEST_CASE("portal config: family estimate filters to the selected family only")
{
	mqttState entities[] = {
		makeEntity("Battery_Voltage", MqttEntityFamily::Battery),
		makeEntity("Battery_Status", MqttEntityFamily::Battery),
		makeEntity("Grid_Power", MqttEntityFamily::Grid),
	};
	entities[0].readKey = 101;
	entities[1].readKey = 101;
	entities[2].readKey = 202;
	const BucketId buckets[] = { BucketId::TenSec, BucketId::TenSec, BucketId::TenSec };

	PortalPollingEstimate estimate = portalBuildFamilyPollingEstimate(
		entities, 3, buckets, MqttEntityFamily::Battery, BucketId::TenSec, 13000, 5000);
	CHECK(estimate.entityCount == 2);
	CHECK(estimate.transactionCount == 1);
	CHECK(estimate.level == PortalEstimateLevel::Light);
}

TEST_CASE("portal config: estimate levels escalate with transaction load")
{
	PortalPollingEstimate idle{};
	CHECK(portalEstimateLevelKey(idle.level) == std::string("idle"));
	CHECK(portalEstimateLevelLabel(idle.level) == std::string("Idle"));

	PortalPollingEstimate tight{};
	tight.entityCount = 1;
	tight.transactionCount = 14;
	tight.estimatedUsedMs = 3500;
	tight.budgetMs = 4000;
	tight.level = PortalEstimateLevel::Tight;
	CHECK(portalEstimateLevelKey(tight.level) == std::string("tight"));
	CHECK(portalEstimateLevelLabel(tight.level) == std::string("Tight"));
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
