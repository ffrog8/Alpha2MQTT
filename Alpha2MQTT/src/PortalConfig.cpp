// Purpose: Keep portal decision logic testable on host (doctest).
#include "../include/PortalConfig.h"

#include "../include/BucketScheduler.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

namespace {
constexpr const char *kPortalMenuIds[] = {
	"wifinoscan",
	"info",
	"custom",
	"update",
	"sep",
	"restart",
};
constexpr char kPortalMenuPolling[] =
	"<form action='/config/mqtt' method='get'><button>MQTT Setup</button></form><br/>\n"
	"<form action='/config/polling' method='get'><button>Polling</button></form><br/>\n";
#if !defined(MP_ESP8266) || !defined(ARDUINO)
constexpr char kPortalRebootNormal[] =
	"<!doctype html><html><head>"
	"<meta charset='utf-8'>"
	"<meta name='viewport' content='width=device-width,initial-scale=1'>"
	"<title>Rebooting…</title>"
	"</head><body>"
	"<h3>Rebooting into normal runtime…</h3>"
	"<p>This page will auto-redirect when runtime mode is available.</p>"
	"<pre id='s'>waiting…</pre>"
	"<script>"
	"(function(){"
	"var start=Date.now();"
	"function tick(){"
	"document.getElementById('s').textContent='waiting '+Math.floor((Date.now()-start)/1000)+'s';"
	"}"
	"async function probe(){"
	"try{"
	"var r=await fetch('/',{cache:'no-store'});"
	"if(r&&r.ok){"
	"var t=await r.text();"
	"if(t&&t.indexOf('Alpha2MQTT Control')>=0){window.location.href='/';return;}"
	"}"
	"}catch(e){}"
	"tick();"
	"setTimeout(probe,1000);"
	"}"
	"setTimeout(probe,1000);"
	"})();"
	"</script>"
	"</body></html>";
#endif

constexpr MqttEntityFamily kPortalPollingFamilies[] = {
	MqttEntityFamily::Battery,
	MqttEntityFamily::Inverter,
	MqttEntityFamily::Backup,
	MqttEntityFamily::Pv,
	MqttEntityFamily::Grid,
	MqttEntityFamily::System,
	MqttEntityFamily::Controller,
};

constexpr uint32_t kPortalEstimateTxnCostMs = 250;

static size_t
countFamilyEntities(const mqttState *entities, size_t entityCount, MqttEntityFamily family)
{
	if (entities == nullptr || entityCount == 0) {
		return 0;
	}

	size_t count = 0;
	for (size_t i = 0; i < entityCount; ++i) {
		if (entities[i].family == family) {
			count++;
		}
	}
	return count;
}

static size_t
countMatchingTransactions(const mqttState *entities,
                          size_t entityCount,
                          const BucketId *buckets,
                          BucketId bucket,
                          MqttEntityFamily *familyFilter)
{
	if (entities == nullptr || buckets == nullptr || entityCount == 0) {
		return 0;
	}

	bool hasSnapshotFanout = false;
	size_t transactionCount = 0;

	for (size_t i = 0; i < entityCount; ++i) {
		if (buckets[i] != bucket) {
			continue;
		}
		if (familyFilter != nullptr && entities[i].family != *familyFilter) {
			continue;
		}

		if (entities[i].needsEssSnapshot) {
			if (!hasSnapshotFanout) {
				hasSnapshotFanout = true;
				transactionCount++;
			}
			continue;
		}

		if (entities[i].readKind == MqttEntityReadKind::Register) {
			bool seenKey = false;
			for (size_t prev = 0; prev < i; ++prev) {
				if (buckets[prev] != bucket) {
					continue;
				}
				if (familyFilter != nullptr && entities[prev].family != *familyFilter) {
					continue;
				}
				if (entities[prev].needsEssSnapshot || entities[prev].readKind != MqttEntityReadKind::Register) {
					continue;
				}
				if (entities[prev].readKey == entities[i].readKey) {
					seenKey = true;
					break;
				}
			}
			if (!seenKey) {
				transactionCount++;
			}
			continue;
		}

		transactionCount++;
	}

	return transactionCount;
}

static PortalEstimateLevel
estimateLevelFor(const PortalPollingEstimate &estimate)
{
	if (estimate.entityCount == 0 || estimate.transactionCount == 0 || estimate.budgetMs == 0) {
		return PortalEstimateLevel::Idle;
	}
	if (estimate.estimatedUsedMs >= estimate.budgetMs) {
		return PortalEstimateLevel::Over;
	}

	const uint32_t usedPct = static_cast<uint32_t>((estimate.estimatedUsedMs * 100UL) / estimate.budgetMs);
	if (usedPct >= 70U) {
		return PortalEstimateLevel::Tight;
	}
	if (usedPct >= 35U) {
		return PortalEstimateLevel::Moderate;
	}
	return PortalEstimateLevel::Light;
}

static PortalRuntimeLevel
runtimeLevelFor(const PortalRuntimeBucketSummary &summary)
{
	if (!summary.observed) {
		return PortalRuntimeLevel::Idle;
	}
	if (!summary.budgetExceeded) {
		return PortalRuntimeLevel::Healthy;
	}
	if (summary.limitMs != 0 && summary.backlogOldestAgeMs > summary.limitMs) {
		return PortalRuntimeLevel::RoutinelyTruncating;
	}
	return PortalRuntimeLevel::Truncating;
}
}

bool
mqttServerIsBlank(const char *server)
{
	return server == nullptr || server[0] == '\0';
}

PortalPostWifiAction
portalPostWifiActionAfterWifiSave(const char *storedMqttServer)
{
	return mqttServerIsBlank(storedMqttServer) ? PortalPostWifiAction::RedirectToMqttParams : PortalPostWifiAction::Reboot;
}

const char *
portalMenuPollingHtml(void)
{
	return kPortalMenuPolling;
}

const char *
portalRebootToNormalHtml(void)
{
#if defined(MP_ESP8266) && defined(ARDUINO)
	return "";
#else
	return kPortalRebootNormal;
#endif
}

uint16_t
portalParseU16Strict(const char *text, uint16_t defaultValue)
{
	if (text == nullptr || text[0] == '\0') {
		return defaultValue;
	}

	char *endPtr = nullptr;
	errno = 0;
	unsigned long parsed = strtoul(text, &endPtr, 10);
	if (errno != 0 || endPtr == text || *endPtr != '\0' || parsed > UINT16_MAX) {
		return defaultValue;
	}
	return static_cast<uint16_t>(parsed);
}

PortalMenu
portalMenuDefault(void)
{
	// WiFiManager's setMenu takes `const char* menu[]` (non-const pointer to const chars).
	// The menu IDs are string literals; returning a casted pointer is safe as long as callers
	// do not try to mutate the array itself.
	return { const_cast<const char **>(kPortalMenuIds),
		static_cast<uint8_t>(sizeof(kPortalMenuIds) / sizeof(kPortalMenuIds[0])) };
}

uint8_t
portalPollingFamilyCount(void)
{
	return static_cast<uint8_t>(sizeof(kPortalPollingFamilies) / sizeof(kPortalPollingFamilies[0]));
}

MqttEntityFamily
portalPollingFamilyAt(uint8_t index)
{
	if (index >= portalPollingFamilyCount()) {
		return kPortalPollingFamilies[0];
	}
	return kPortalPollingFamilies[index];
}

const char *
portalPollingFamilyKey(MqttEntityFamily family)
{
	switch (family) {
	case MqttEntityFamily::Battery:
		return "battery";
	case MqttEntityFamily::Inverter:
		return "inverter";
	case MqttEntityFamily::Backup:
		return "backup";
	case MqttEntityFamily::Pv:
		return "pv";
	case MqttEntityFamily::Grid:
		return "grid";
	case MqttEntityFamily::System:
		return "system";
	case MqttEntityFamily::Controller:
	default:
		return "controller";
	}
}

const char *
portalPollingFamilyLabel(MqttEntityFamily family)
{
	switch (family) {
	case MqttEntityFamily::Battery:
		return "Battery";
	case MqttEntityFamily::Inverter:
		return "Inverter";
	case MqttEntityFamily::Backup:
		return "Backup";
	case MqttEntityFamily::Pv:
		return "PV";
	case MqttEntityFamily::Grid:
		return "Grid";
	case MqttEntityFamily::System:
		return "System";
	case MqttEntityFamily::Controller:
	default:
		return "Controller";
	}
}

bool
portalPollingFamilyFromKey(const char *key, MqttEntityFamily *outFamily)
{
	if (key == nullptr || key[0] == '\0' || outFamily == nullptr) {
		return false;
	}

	for (uint8_t i = 0; i < portalPollingFamilyCount(); ++i) {
		const MqttEntityFamily family = portalPollingFamilyAt(i);
		if (strcmp(portalPollingFamilyKey(family), key) == 0) {
			*outFamily = family;
			return true;
		}
	}
	return false;
}

MqttEntityFamily
portalNormalizePollingFamily(const mqttState *entities,
                             size_t entityCount,
                             const char *requestedKey)
{
	MqttEntityFamily requestedFamily = portalPollingFamilyAt(0);
	if (portalPollingFamilyFromKey(requestedKey, &requestedFamily) &&
	    countFamilyEntities(entities, entityCount, requestedFamily) > 0) {
		return requestedFamily;
	}

	for (uint8_t i = 0; i < portalPollingFamilyCount(); ++i) {
		const MqttEntityFamily family = portalPollingFamilyAt(i);
		if (countFamilyEntities(entities, entityCount, family) > 0) {
			return family;
		}
	}

	return portalPollingFamilyAt(0);
}

PortalPollingEstimate
portalBuildPollingEstimate(const mqttState *entities,
                           size_t entityCount,
                           const BucketId *buckets,
                           BucketId bucket,
                           uint32_t userIntervalMs,
                           uint32_t maxBudgetMs)
{
	PortalPollingEstimate estimate{};
	estimate.bucketId = bucket;
	if (entities == nullptr || buckets == nullptr || entityCount == 0) {
		return estimate;
	}

	for (size_t i = 0; i < entityCount; ++i) {
		if (buckets[i] == bucket) {
			estimate.entityCount++;
		}
	}
	estimate.transactionCount = countMatchingTransactions(entities, entityCount, buckets, bucket, nullptr);
	estimate.budgetMs = bucketBudgetMs(bucket, userIntervalMs, maxBudgetMs);
	estimate.estimatedUsedMs = static_cast<uint32_t>(estimate.transactionCount * kPortalEstimateTxnCostMs);
	estimate.level = estimateLevelFor(estimate);
	return estimate;
}

PortalPollingEstimate
portalBuildFamilyPollingEstimate(const mqttState *entities,
                                 size_t entityCount,
                                 const BucketId *buckets,
                                 MqttEntityFamily family,
                                 BucketId bucket,
                                 uint32_t userIntervalMs,
                                 uint32_t maxBudgetMs)
{
	PortalPollingEstimate estimate{};
	estimate.bucketId = bucket;
	if (entities == nullptr || buckets == nullptr || entityCount == 0) {
		return estimate;
	}

	for (size_t i = 0; i < entityCount; ++i) {
		if (buckets[i] == bucket && entities[i].family == family) {
			estimate.entityCount++;
		}
	}
	estimate.transactionCount = countMatchingTransactions(entities, entityCount, buckets, bucket, &family);
	estimate.budgetMs = bucketBudgetMs(bucket, userIntervalMs, maxBudgetMs);
	estimate.estimatedUsedMs = static_cast<uint32_t>(estimate.transactionCount * kPortalEstimateTxnCostMs);
	estimate.level = estimateLevelFor(estimate);
	return estimate;
}

const char *
portalEstimateLevelKey(PortalEstimateLevel level)
{
	switch (level) {
	case PortalEstimateLevel::Light:
		return "light";
	case PortalEstimateLevel::Moderate:
		return "moderate";
	case PortalEstimateLevel::Tight:
		return "tight";
	case PortalEstimateLevel::Over:
		return "over";
	case PortalEstimateLevel::Idle:
	default:
		return "idle";
	}
}

const char *
portalEstimateLevelLabel(PortalEstimateLevel level)
{
	switch (level) {
	case PortalEstimateLevel::Light:
		return "Light";
	case PortalEstimateLevel::Moderate:
		return "Moderate";
	case PortalEstimateLevel::Tight:
		return "Tight";
	case PortalEstimateLevel::Over:
		return "Over";
	case PortalEstimateLevel::Idle:
	default:
		return "Idle";
	}
}

PortalRuntimeBucketSummary
portalBuildRuntimeBucketSummary(BucketId bucketId,
                                const BucketRuntimeBudgetState &state,
                                uint32_t nowMs)
{
	PortalRuntimeBucketSummary summary{};
	summary.bucketId = bucketId;
	summary.observed = state.observed;
	summary.budgetExceeded = bucketRuntimeBudgetExceeded(state);
	summary.usedMs = state.usedMsLast;
	summary.limitMs = state.limitMsLast;
	summary.backlogCount = state.backlogCount;
	summary.backlogOldestAgeMs = bucketBacklogOldestAgeMs(state, nowMs);
	summary.lastFullCycleAgeMs = bucketLastFullCycleAgeMs(state, nowMs);
	summary.level = runtimeLevelFor(summary);
	return summary;
}

const char *
portalRuntimeLevelKey(PortalRuntimeLevel level)
{
	switch (level) {
	case PortalRuntimeLevel::Healthy:
		return "healthy";
	case PortalRuntimeLevel::Truncating:
		return "truncating";
	case PortalRuntimeLevel::RoutinelyTruncating:
		return "routine";
	case PortalRuntimeLevel::Idle:
	default:
		return "idle";
	}
}

const char *
portalRuntimeLevelLabel(PortalRuntimeLevel level)
{
	switch (level) {
	case PortalRuntimeLevel::Healthy:
		return "Healthy";
	case PortalRuntimeLevel::Truncating:
		return "Truncating";
	case PortalRuntimeLevel::RoutinelyTruncating:
		return "Routinely truncating";
	case PortalRuntimeLevel::Idle:
	default:
		return "Idle";
	}
}

PortalFamilyPage
portalBuildFamilyPage(const mqttState *entities,
                      size_t entityCount,
                      MqttEntityFamily family,
                      uint16_t requestedPage,
                      size_t pageSize)
{
	PortalFamilyPage page{};
	page.family = family;

	if (pageSize == 0) {
		return page;
	}

	page.totalEntityCount = countFamilyEntities(entities, entityCount, family);
	if (page.totalEntityCount == 0) {
		return page;
	}

	page.maxPage = static_cast<uint16_t>((page.totalEntityCount - 1) / pageSize);
	page.safePage = (requestedPage > page.maxPage) ? page.maxPage : requestedPage;
	page.pageStartOffset = static_cast<size_t>(page.safePage) * pageSize;
	page.pageCount = page.totalEntityCount - page.pageStartOffset;
	if (page.pageCount > pageSize) {
		page.pageCount = pageSize;
	}
	return page;
}

size_t
portalCollectFamilyPageEntityIndices(const mqttState *entities,
                                     size_t entityCount,
                                     const PortalFamilyPage &page,
                                     uint16_t *outIndices,
                                     size_t outCapacity)
{
	if (entities == nullptr || outIndices == nullptr || outCapacity == 0 || page.pageCount == 0) {
		return 0;
	}

	const size_t wanted = (page.pageCount < outCapacity) ? page.pageCount : outCapacity;
	size_t familyOrdinal = 0;
	size_t collected = 0;
	for (size_t i = 0; i < entityCount && collected < wanted; ++i) {
		if (entities[i].family != page.family) {
			continue;
		}
		if (familyOrdinal >= page.pageStartOffset) {
			outIndices[collected++] = static_cast<uint16_t>(i);
		}
		familyOrdinal++;
	}
	return collected;
}

void
portalSetAllBuckets(BucketId *buckets, size_t entityCount, BucketId bucket)
{
	if (buckets == nullptr || entityCount == 0) {
		return;
	}
	for (size_t i = 0; i < entityCount; ++i) {
		buckets[i] = bucket;
	}
}
