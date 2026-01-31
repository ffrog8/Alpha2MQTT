// Purpose: Host-testable helpers for portal save/redirect decisions.
// Invariants: Pure logic only; no Arduino/WiFiManager dependencies.
#pragma once

enum class PortalPostWifiAction {
	Reboot,
	RedirectToMqttParams
};

bool mqttServerIsBlank(const char *server);
PortalPostWifiAction portalPostWifiActionAfterWifiSave(const char *storedMqttServer);

