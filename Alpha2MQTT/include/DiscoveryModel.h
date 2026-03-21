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

// Writes "A2M-<serial>" when serial is valid, else writes "A2M-UNKNOWN".
void buildInverterHaUniqueId(const char *serial, char *out, size_t outLen);

// Returns true when the current HA unique_id already matches the inverter serial.
bool inverterHaUniqueIdMatchesSerial(const char *uniqueId, const char *serial);

// Returns true for non-empty serial strings that look like real inverter IDs.
// The helper intentionally rejects placeholder values and obvious invalid
// placeholders (for example, all-zero strings or bare "unknown").
bool inverterSerialIsValid(const char *serial);

// Writes the effective display label for inverter naming. Prefers the user
// override when present, otherwise uses the raw last 3 chars of the serial.
bool buildInverterLabelDisplay(const char *serial,
                               const char *labelOverride,
                               char *out,
                               size_t outLen);

// Normalizes a display label into a lowercase snake_case id segment.
void buildInverterLabelId(const char *labelDisplay, char *out, size_t outLen);

// Returns true when a user-supplied label can be used in discovery payloads
// and still normalizes to a non-empty entity-id segment.
bool inverterLabelOverrideIsValid(const char *labelOverride);

// Writes the HA-facing inverter device display name: "Alpha <label>".
bool buildInverterDeviceDisplayName(const char *serial,
                                    const char *labelOverride,
                                    char *out,
                                    size_t outLen);

// Writes the previous inverter discovery identifier when the serial changed
// from one valid value to another. Returns false when there is no stale
// inverter namespace to clear.
bool buildStaleInverterIdentifier(const char *previousSerial,
                                  const char *nextSerial,
                                  char *out,
                                  size_t outLen);

// Returns device scope for a compiled MQTT entity.
DiscoveryDeviceScope mqttEntityScope(mqttEntityId id);

// Writes the HA-facing metric id used for inverter entity ids / unique ids.
void buildEntityMetricId(const mqttState *entity, char *out, size_t outLen);

// Writes the HA-facing metric display name. Inverter names are label-agnostic;
// Home Assistant combines them with the inverter device name.
void buildEntityDisplayName(const mqttState *entity,
                            DiscoveryDeviceScope scope,
                            char *out,
                            size_t outLen);

// Writes stable unique_id: "<controller|inverter id>_<metric key>".
void buildEntityUniqueId(DiscoveryDeviceScope scope,
                         const char *controllerId,
                         const char *serial,
                         const char *metricKey,
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
