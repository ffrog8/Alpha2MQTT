// Purpose: Provide deterministic MQTT discovery identity/scope helpers shared by firmware and host tests.
// Invariants: Controller identity is stable from MAC, inverter identity requires a valid serial.
#pragma once

#include <cstddef>
#include <cstdint>

#include "Definitions.h"

enum class DiscoveryDeviceScope : uint8_t {
	Controller = 0,
	Inverter = 1
};

// Writes "alpha2mqtt_<mac>" using lower-case hex and no separators.
void buildControllerIdentifier(const uint8_t mac[6], char *out, size_t outLen);

// Writes "alpha2mqtt_inv_<serial>" when serial is valid, else writes empty string.
void buildInverterIdentifier(const char *serial, char *out, size_t outLen);

// Returns true only for non-empty serials that are not placeholder values.
bool inverterSerialIsValid(const char *serial);

// Returns device scope for a compiled MQTT entity.
DiscoveryDeviceScope mqttEntityScope(mqttEntityId id);

// Writes stable unique_id: "<controller|inverter id>_<entity key>".
void buildEntityUniqueId(DiscoveryDeviceScope scope,
                         const char *controllerId,
                         const char *serial,
                         const char *entityKey,
                         char *out,
                         size_t outLen);

// Writes state/command topic base: "<deviceName>/<controller|inverter id>/<entity key>".
// Returns false when scope is inverter and serial is not valid.
bool buildEntityTopicBase(const char *deviceName,
                          DiscoveryDeviceScope scope,
                          const char *controllerId,
                          const char *serial,
                          const char *entityKey,
                          char *out,
                          size_t outLen);
