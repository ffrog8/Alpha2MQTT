// Purpose: Keep MQTT discovery identity generation and entity scope mapping deterministic and heap-free.
#include "../include/DiscoveryModel.h"

#include <cstdio>
#include <cstring>

void
buildControllerIdentifier(const uint8_t mac[6], char *out, size_t outLen)
{
	if (out == nullptr || outLen == 0) {
		return;
	}
	if (mac == nullptr) {
		out[0] = '\0';
		return;
	}
	snprintf(out, outLen, "alpha2mqtt_%02x%02x%02x%02x%02x%02x",
	         mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

bool
inverterSerialIsValid(const char *serial)
{
	if (serial == nullptr || serial[0] == '\0') {
		return false;
	}
	if (strcmp(serial, "A2M-UNKNOWN") == 0 || strcmp(serial, "UNKNOWN") == 0 || strcmp(serial, "unknown") == 0) {
		return false;
	}
	return true;
}

void
buildInverterIdentifier(const char *serial, char *out, size_t outLen)
{
	if (out == nullptr || outLen == 0) {
		return;
	}
	if (!inverterSerialIsValid(serial)) {
		out[0] = '\0';
		return;
	}
	snprintf(out, outLen, "alpha2mqtt_inv_%s", serial);
}

DiscoveryDeviceScope
mqttEntityScope(mqttEntityId id)
{
	switch (id) {
#ifdef DEBUG_FREEMEM
	case mqttEntityId::entityFreemem:
#endif
#ifdef DEBUG_CALLBACKS
	case mqttEntityId::entityCallbacks:
#endif
#ifdef A2M_DEBUG_WIFI
	case mqttEntityId::entityRSSI:
	case mqttEntityId::entityBSSID:
	case mqttEntityId::entityTxPower:
	case mqttEntityId::entityWifiRecon:
#endif
#ifdef DEBUG_RS485
	case mqttEntityId::entityRs485Errors:
#endif
	case mqttEntityId::entityRs485Avail:
	case mqttEntityId::entityA2MUptime:
	case mqttEntityId::entityA2MVersion:
		return DiscoveryDeviceScope::Controller;
	default:
		return DiscoveryDeviceScope::Inverter;
	}
}

void
buildEntityUniqueId(DiscoveryDeviceScope scope,
                    const char *controllerId,
                    const char *serial,
                    const char *entityKey,
                    char *out,
                    size_t outLen)
{
	if (out == nullptr || outLen == 0) {
		return;
	}
	const char *key = entityKey ? entityKey : "";
	if (scope == DiscoveryDeviceScope::Controller) {
		snprintf(out, outLen, "%s_%s", controllerId ? controllerId : "", key);
		return;
	}

	char inverterId[64];
	buildInverterIdentifier(serial, inverterId, sizeof(inverterId));
	snprintf(out, outLen, "%s_%s", inverterId, key);
}

bool
buildEntityTopicBase(const char *deviceName,
                     DiscoveryDeviceScope scope,
                     const char *controllerId,
                     const char *serial,
                     const char *entityKey,
                     char *out,
                     size_t outLen)
{
	if (out == nullptr || outLen == 0) {
		return false;
	}
	if (scope == DiscoveryDeviceScope::Controller) {
		snprintf(out, outLen, "%s/%s/%s",
		         deviceName ? deviceName : "",
		         controllerId ? controllerId : "",
		         entityKey ? entityKey : "");
		return true;
	}

	char inverterId[64];
	buildInverterIdentifier(serial, inverterId, sizeof(inverterId));
	if (inverterId[0] == '\0') {
		out[0] = '\0';
		return false;
	}
	snprintf(out, outLen, "%s/%s/%s",
	         deviceName ? deviceName : "",
	         inverterId,
	         entityKey ? entityKey : "");
	return true;
}
