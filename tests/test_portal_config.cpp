// Purpose: Lock down portal save/redirect rules so WiFi/MQTT persistence changes
// don't regress when portal behavior is adjusted.

#include <doctest/doctest.h>

#include <cstdio>
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

TEST_CASE("portal config: mqtt completeness requires server and port only")
{
	CHECK_FALSE(mqttConfigIsComplete(nullptr, 1883, "user", "pass"));
	CHECK_FALSE(mqttConfigIsComplete("", 1883, "user", "pass"));
	CHECK_FALSE(mqttConfigIsComplete("broker", 0, "user", "pass"));
	CHECK(mqttConfigIsComplete("broker", 1883, "", ""));
	CHECK(mqttConfigIsComplete("broker", 1883, "user", ""));
	CHECK(mqttConfigIsComplete("broker", 1883, "", "pass"));
	CHECK(mqttConfigIsComplete("broker", 1883, "user", "pass"));
}

TEST_CASE("portal config: post-wifi action redirects until mqtt config is complete")
{
	CHECK(portalPostWifiActionAfterWifiSave("", 1883, "user", "pass") ==
	      PortalPostWifiAction::RedirectToMqttParams);
	CHECK(portalPostWifiActionAfterWifiSave("broker", 0, "user", "pass") ==
	      PortalPostWifiAction::RedirectToMqttParams);
	CHECK(portalPostWifiActionAfterWifiSave("broker", 1883, "", "pass") ==
	      PortalPostWifiAction::Reboot);
	CHECK(portalPostWifiActionAfterWifiSave("broker", 1883, "user", "") ==
	      PortalPostWifiAction::Reboot);
	CHECK(portalPostWifiActionAfterWifiSave("broker", 1883, "user", "pass") ==
	      PortalPostWifiAction::Reboot);
}

TEST_CASE("portal config: wifi save keeps saved password only for same-ssid blank saves")
{
	CHECK(portalWifiSaveKeepsExistingPassword("home", "home", "", false));
	CHECK_FALSE(portalWifiSaveKeepsExistingPassword("home", "guest", "", false));
	CHECK_FALSE(portalWifiSaveKeepsExistingPassword("home", "home", "newpass", false));
	CHECK_FALSE(portalWifiSaveKeepsExistingPassword("home", "home", "", true));
	CHECK_FALSE(portalWifiSaveKeepsExistingPassword("", "home", "", false));
}

TEST_CASE("portal config: blank wifi passwords require explicit open-network intent for a different ssid")
{
	CHECK(portalWifiSaveAllowsBlankPassword("home", "home", "", false));
	CHECK(portalWifiSaveAllowsBlankPassword("home", "guest", "newpass", false));
	CHECK(portalWifiSaveAllowsBlankPassword("home", "guest", "", true));
	CHECK_FALSE(portalWifiSaveAllowsBlankPassword("home", "guest", "", false));
}

TEST_CASE("portal config: menu includes update and no exit")
{
	PortalMenu menu = portalMenuDefault();
	CHECK(menu.items != nullptr);
	CHECK(menu.count > 0);

	bool hasUpdate = false;
	bool hasExit = false;
	bool hasCustom = false;
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
	}

	CHECK(hasUpdate);
	CHECK(hasCustom);
	CHECK(!hasExit);
	CHECK(customIndex >= 0);
	CHECK(updateIndex > customIndex);
}

TEST_CASE("portal config: custom menu html points to mqtt and polling pages")
{
	const char *html = portalMenuPollingHtml();
	REQUIRE(html != nullptr);
	CHECK(strstr(html, "/config/mqtt") != nullptr);
	CHECK(strstr(html, "MQTT Setup") != nullptr);
	CHECK(strstr(html, "/config/polling") != nullptr);
	CHECK(strstr(html, "Polling") != nullptr);
}

TEST_CASE("portal config: sta custom menu html includes wifi, mqtt, and polling pages")
{
	const char *html = portalMenuStaHtml();
	REQUIRE(html != nullptr);
	CHECK(strstr(html, "/0wifi") != nullptr);
	CHECK(strstr(html, "WiFi Setup") != nullptr);
	CHECK(strstr(html, "/config/mqtt") != nullptr);
	CHECK(strstr(html, "/config/polling") != nullptr);
}

TEST_CASE("portal config: reboot-to-normal html includes runtime redirect probe")
{
	const char *html = portalRebootToNormalHtml();
	REQUIRE(html != nullptr);
	CHECK(strstr(html, "Rebooting into normal runtime") != nullptr);
	CHECK(strstr(html, "fetch('/',{cache:'no-store'})") != nullptr);
	CHECK(strstr(html, "Alpha2MQTT Control") != nullptr);
}

TEST_CASE("portal config: strict uint16 parser rejects malformed and overflowing values")
{
	CHECK(portalParseU16Strict(nullptr, 7) == 7);
	CHECK(portalParseU16Strict("", 7) == 7);
	CHECK(portalParseU16Strict("12", 7) == 12);
	CHECK(portalParseU16Strict("1abc", 7) == 7);
	CHECK(portalParseU16Strict("70000", 7) == 7);
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

TEST_CASE("portal config: family paging collects up to eight entities per page")
{
	mqttState entities[10]{};
	for (size_t i = 0; i < 10; ++i) {
		char name[24];
		snprintf(name, sizeof(name), "Battery_%zu", i);
		entities[i] = makeEntity(name, MqttEntityFamily::Battery);
	}

	PortalFamilyPage first = portalBuildFamilyPage(entities, 10, MqttEntityFamily::Battery, 0, 8);
	CHECK(first.safePage == 0);
	CHECK(first.maxPage == 1);
	CHECK(first.totalEntityCount == 10);
	CHECK(first.pageStartOffset == 0);
	CHECK(first.pageCount == 8);

	uint16_t firstIndices[8]{};
	CHECK(portalCollectFamilyPageEntityIndices(entities, 10, first, firstIndices, 8) == 8);
	CHECK(firstIndices[0] == 0);
	CHECK(firstIndices[7] == 7);

	PortalFamilyPage last = portalBuildFamilyPage(entities, 10, MqttEntityFamily::Battery, 99, 8);
	CHECK(last.safePage == 1);
	CHECK(last.maxPage == 1);
	CHECK(last.pageStartOffset == 8);
	CHECK(last.pageCount == 2);

	uint16_t lastIndices[8]{};
	CHECK(portalCollectFamilyPageEntityIndices(entities, 10, last, lastIndices, 8) == 2);
	CHECK(lastIndices[0] == 8);
	CHECK(lastIndices[1] == 9);
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

TEST_CASE("portal config: runtime summary reports healthy buckets")
{
	BucketRuntimeBudgetState state{};
	state.observed = true;
	state.usedMsLast = 1800;
	state.limitMsLast = 5000;
	state.lastFullCycleCompletedMs = 9000;

	const PortalRuntimeBucketSummary summary = portalBuildRuntimeBucketSummary(BucketId::TenSec, state, 10000);
	CHECK(summary.bucketId == BucketId::TenSec);
	CHECK(summary.observed);
	CHECK_FALSE(summary.budgetExceeded);
	CHECK(summary.usedMs == 1800);
	CHECK(summary.limitMs == 5000);
	CHECK(summary.backlogCount == 0);
	CHECK(summary.lastFullCycleAgeMs == 1000);
	CHECK(summary.level == PortalRuntimeLevel::Healthy);
	CHECK(portalRuntimeLevelKey(summary.level) == std::string("healthy"));
	CHECK(portalRuntimeLevelLabel(summary.level) == std::string("Healthy"));
}

TEST_CASE("portal config: runtime summary flags routine truncation")
{
	BucketRuntimeBudgetState state{};
	state.observed = true;
	state.usedMsLast = 5200;
	state.limitMsLast = 5000;
	state.backlogCount = 3;
	state.deferredSinceMs = 1000;
	state.lastFullCycleCompletedMs = 250;

	const PortalRuntimeBucketSummary summary = portalBuildRuntimeBucketSummary(BucketId::OneMin, state, 8000);
	CHECK(summary.budgetExceeded);
	CHECK(summary.backlogCount == 3);
	CHECK(summary.backlogOldestAgeMs == 7000);
	CHECK(summary.lastFullCycleAgeMs == 7750);
	CHECK(summary.level == PortalRuntimeLevel::RoutinelyTruncating);
	CHECK(portalRuntimeLevelKey(summary.level) == std::string("routine"));
	CHECK(portalRuntimeLevelLabel(summary.level) == std::string("Routinely truncating"));
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

TEST_CASE("portal config: set all buckets assigns every entity slot")
{
	BucketId buckets[] = {
		BucketId::TenSec,
		BucketId::OneMin,
		BucketId::User,
	};

	portalSetAllBuckets(buckets, 3, BucketId::Disabled);
	CHECK(buckets[0] == BucketId::Disabled);
	CHECK(buckets[1] == BucketId::Disabled);
	CHECK(buckets[2] == BucketId::Disabled);

	portalSetAllBuckets(nullptr, 3, BucketId::TenSec);
	portalSetAllBuckets(buckets, 0, BucketId::TenSec);
	CHECK(buckets[0] == BucketId::Disabled);
}
