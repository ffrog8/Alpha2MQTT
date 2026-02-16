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

PortalMenu
portalMenuDefault(void)
{
	// WiFiManager's setMenu takes `const char* menu[]` (non-const pointer to const chars).
	// The menu IDs are string literals; returning a casted pointer is safe as long as callers
	// do not try to mutate the array itself.
	return { const_cast<const char **>(kPortalMenuIds),
		static_cast<uint8_t>(sizeof(kPortalMenuIds) / sizeof(kPortalMenuIds[0])) };
}
