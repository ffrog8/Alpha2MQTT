// Purpose: Keep portal decision logic testable on host (doctest).
#include "../include/PortalConfig.h"

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
	return kPortalRebootNormal;
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
