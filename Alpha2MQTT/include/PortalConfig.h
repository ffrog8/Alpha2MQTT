// Purpose: Host-testable helpers for portal save/redirect decisions.
// Invariants: Pure logic only; no Arduino/WiFiManager dependencies.
#pragma once

#include <cstdint>

enum class PortalPostWifiAction {
	Reboot,
	RedirectToMqttParams
};

bool mqttServerIsBlank(const char *server);
PortalPostWifiAction portalPostWifiActionAfterWifiSave(const char *storedMqttServer);
const char *portalMenuPollingHtml(void);
const char *portalRebootToNormalHtml(void);

struct PortalMenu {
	const char **items;
	uint8_t count;
};

// WiFiManager menu IDs used by both AP and STA portal modes.
PortalMenu portalMenuDefault(void);
