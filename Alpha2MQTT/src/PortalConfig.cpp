// Purpose: Keep portal decision logic testable on host (doctest).
#include "../include/PortalConfig.h"

#include <cstring>

namespace {
constexpr const char *kPortalMenuIds[] = {
	"wifinoscan",
	"info",
	"param",
	"custom",
	"update",
	"sep",
	"restart",
};
constexpr char kPortalMenuPolling[] =
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
