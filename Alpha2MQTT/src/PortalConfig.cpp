// Purpose: Keep portal decision logic testable on host (doctest).
#include "../include/PortalConfig.h"

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

