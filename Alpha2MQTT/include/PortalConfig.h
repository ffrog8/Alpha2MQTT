// Purpose: Host-testable helpers for portal save/redirect decisions and
// family-first polling portal navigation.
// Invariants: Pure logic only; no Arduino/WiFiManager dependencies.
#pragma once

#include <cstddef>
#include <cstdint>

#include "Definitions.h"

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
