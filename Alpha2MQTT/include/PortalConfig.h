// Purpose: Host-testable helpers for portal save/redirect decisions and
// family-first polling portal navigation.
// Invariants: Pure logic only; no Arduino/WiFiManager dependencies.
#pragma once

#include <cstddef>
#include <cstdint>

#include "Definitions.h"
#include "Scheduler.h"

enum class PortalPostWifiAction {
	Reboot,
	RedirectToMqttParams
};

enum class PortalEstimateLevel : uint8_t {
	Idle = 0,
	Light,
	Moderate,
	Tight,
	Over
};

enum class PortalRuntimeLevel : uint8_t {
	Idle = 0,
	Healthy,
	Truncating,
	RoutinelyTruncating
};

struct PortalPollingEstimate {
	BucketId bucketId = BucketId::Disabled;
	size_t entityCount = 0;
	size_t transactionCount = 0;
	uint32_t estimatedUsedMs = 0;
	uint32_t budgetMs = 0;
	PortalEstimateLevel level = PortalEstimateLevel::Idle;
};

struct PortalRuntimeBucketSummary {
	BucketId bucketId = BucketId::Disabled;
	bool observed = false;
	bool budgetExceeded = false;
	uint32_t usedMs = 0;
	uint32_t limitMs = 0;
	uint16_t backlogCount = 0;
	uint32_t backlogOldestAgeMs = 0;
	uint32_t lastFullCycleAgeMs = 0;
	PortalRuntimeLevel level = PortalRuntimeLevel::Idle;
};

bool mqttServerIsBlank(const char *server);
bool mqttConfigIsComplete(const char *server, uint16_t port, const char *user, const char *password);
PortalPostWifiAction portalPostWifiActionAfterWifiSave(const char *server,
                                                       uint16_t port,
                                                       const char *user,
                                                       const char *password);
const char *portalMenuPollingHtml(void);
const char *portalMenuStaHtml(void);
const char *portalRebootToNormalHtml(void);
uint16_t portalParseU16Strict(const char *text, uint16_t defaultValue);

struct PortalMenu {
	const char **items;
	uint8_t count;
};

struct PortalFamilyPage {
	MqttEntityFamily family = MqttEntityFamily::Battery;
	uint16_t safePage = 0;
	uint16_t maxPage = 0;
	size_t totalEntityCount = 0;
	size_t pageStartOffset = 0;
	size_t pageCount = 0;
};

// WiFiManager menu IDs used by both AP and STA portal modes.
PortalMenu portalMenuDefault(void);
uint8_t portalPollingFamilyCount(void);
MqttEntityFamily portalPollingFamilyAt(uint8_t index);
const char *portalPollingFamilyKey(MqttEntityFamily family);
const char *portalPollingFamilyLabel(MqttEntityFamily family);
bool portalPollingFamilyFromKey(const char *key, MqttEntityFamily *outFamily);
MqttEntityFamily portalNormalizePollingFamily(const mqttState *entities,
                                             size_t entityCount,
                                             const char *requestedKey);
PortalPollingEstimate portalBuildPollingEstimate(const mqttState *entities,
                                                 size_t entityCount,
                                                 const BucketId *buckets,
                                                 BucketId bucket,
                                                 uint32_t userIntervalMs,
                                                 uint32_t maxBudgetMs);
PortalPollingEstimate portalBuildFamilyPollingEstimate(const mqttState *entities,
                                                       size_t entityCount,
                                                       const BucketId *buckets,
                                                       MqttEntityFamily family,
                                                       BucketId bucket,
                                                       uint32_t userIntervalMs,
                                                       uint32_t maxBudgetMs);
const char *portalEstimateLevelKey(PortalEstimateLevel level);
const char *portalEstimateLevelLabel(PortalEstimateLevel level);
PortalRuntimeBucketSummary portalBuildRuntimeBucketSummary(BucketId bucketId,
                                                          const BucketRuntimeBudgetState &state,
                                                          uint32_t nowMs);
const char *portalRuntimeLevelKey(PortalRuntimeLevel level);
const char *portalRuntimeLevelLabel(PortalRuntimeLevel level);
PortalFamilyPage portalBuildFamilyPage(const mqttState *entities,
                                       size_t entityCount,
                                       MqttEntityFamily family,
                                       uint16_t requestedPage,
                                       size_t pageSize);
size_t portalCollectFamilyPageEntityIndices(const mqttState *entities,
                                            size_t entityCount,
                                            const PortalFamilyPage &page,
                                            uint16_t *outIndices,
                                            size_t outCapacity);
void portalSetAllBuckets(BucketId *buckets, size_t entityCount, BucketId bucket);
