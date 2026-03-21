// Purpose: Keep MQTT discovery identity generation and entity scope mapping deterministic and heap-free.
#include "../include/DiscoveryModel.h"
#include "../include/MqttEntities.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace {

struct MetricNameOverride {
	mqttEntityId entityId;
	const char *metricId;
	const char *displayName;
};

const MetricNameOverride kMetricNameOverrides[] = {
	{ mqttEntityId::entityInverterSn, "inverter_serial", "Inverter Serial" },
	{ mqttEntityId::entityInverterVersion, "inverter_version", "Inverter Version" },
	{ mqttEntityId::entityEmsSn, "ems_serial", "EMS Serial" },
	{ mqttEntityId::entityEmsVersion, "ems_version", "EMS Version" },
	{ mqttEntityId::entityBatSoc, "battery_soc", "Battery SOC" },
	{ mqttEntityId::entityBatPwr, "battery_power", "Battery Power" },
	{ mqttEntityId::entityBatTemp, "battery_temperature", "Battery Temperature" },
	{ mqttEntityId::entityInverterTemp, "inverter_temperature", "Inverter Temperature" },
	{ mqttEntityId::entityPvMeterPowerTotal, "solar_ct_power", "Solar CT Power" },
	{ mqttEntityId::entityPv1Power, "solar_pv1_power", "Solar PV1 Power" },
	{ mqttEntityId::entityPv2Power, "solar_pv2_power", "Solar PV2 Power" },
	{ mqttEntityId::entityBatFaults, "battery_fault", "Battery Fault" },
	{ mqttEntityId::entitySystemFaults, "system_fault", "System Fault" },
};

const MetricNameOverride *
findMetricNameOverride(mqttEntityId entityId)
{
	for (const auto &entry : kMetricNameOverrides) {
		if (entry.entityId == entityId) {
			return &entry;
		}
	}
	return nullptr;
}

void
normalizeIdSegment(const char *src, char *out, size_t outLen)
{
	if (out == nullptr || outLen == 0) {
		return;
	}
	out[0] = '\0';
	if (src == nullptr || src[0] == '\0') {
		return;
	}

	size_t write = 0;
	bool lastUnderscore = false;
	for (size_t i = 0; src[i] != '\0' && write + 1 < outLen; ++i) {
		const unsigned char ch = static_cast<unsigned char>(src[i]);
		if (std::isalnum(ch)) {
			out[write++] = static_cast<char>(std::tolower(ch));
			lastUnderscore = false;
			continue;
		}
		if (!lastUnderscore && write > 0) {
			out[write++] = '_';
			lastUnderscore = true;
		}
	}
	while (write > 0 && out[write - 1] == '_') {
		--write;
	}
	out[write] = '\0';
}

void
underscoreNameToDisplay(const char *src, char *out, size_t outLen)
{
	if (out == nullptr || outLen == 0) {
		return;
	}
	out[0] = '\0';
	if (src == nullptr || src[0] == '\0') {
		return;
	}
	size_t write = 0;
	bool capitalize = true;
	for (size_t i = 0; src[i] != '\0' && write + 1 < outLen; ++i) {
		char ch = src[i];
		if (ch == '_') {
			if (write > 0 && out[write - 1] != ' ' && write + 1 < outLen) {
				out[write++] = ' ';
			}
			capitalize = true;
			continue;
		}
		if (capitalize && std::islower(static_cast<unsigned char>(ch))) {
			ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
		}
		out[write++] = ch;
		capitalize = (ch == ' ');
	}
	out[write] = '\0';
}

} // namespace

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
	size_t len = 0;
	while (len < 17 && serial[len] != '\0') {
		++len;
	}
	if (len < 6 || len > 16) {
		return false;
	}

	// Reject known placeholders and transport-only payloads.
	if (strcmp(serial, "A2M-UNKNOWN") == 0 || strcmp(serial, "UNKNOWN") == 0 || strcmp(serial, "unknown") == 0) {
		return false;
	}

	bool hasLetter = false;
	bool hasDigit = false;
	bool allDigits = true;
	bool allZero = true;
	char first = serial[0];
	bool allSame = true;
	for (size_t i = 0; i < len; ++i) {
		const unsigned char ch = static_cast<unsigned char>(serial[i]);
		if (!std::isalnum(ch)) {
			return false;
		}
		if (std::isalpha(ch)) {
			hasLetter = true;
			allDigits = false;
		} else {
			hasDigit = true;
		}
		if (ch != '0') {
			allZero = false;
		}
		if (serial[i] != first) {
			allSame = false;
		}
	}

	// Serial values should look like mixed ID tokens, not raw placeholders.
	if (allDigits || !hasLetter || (!hasDigit && len >= 6 && strncmp(serial, "STUBSN", 6) != 0)) {
		return false;
	}
	if (strncmp(serial, "STUBSN", 6) == 0) {
		// Allow test-mode/fixture stubs that intentionally include all-zero suffixes.
		return true;
	}
	if (allZero || allSame) {
		return false;
	}
	return true;
}

bool
buildInverterLabelDisplay(const char *serial,
                          const char *labelOverride,
                          char *out,
                          size_t outLen)
{
	if (out == nullptr || outLen == 0) {
		return false;
	}
	out[0] = '\0';
	if (labelOverride != nullptr && labelOverride[0] != '\0') {
		snprintf(out, outLen, "%s", labelOverride);
		return true;
	}
	if (!inverterSerialIsValid(serial)) {
		return false;
	}
	const size_t serialLen = strlen(serial);
	const char *tail = serial + (serialLen > 3 ? (serialLen - 3) : 0);
	snprintf(out, outLen, "%s", tail);
	return out[0] != '\0';
}

void
buildInverterLabelId(const char *labelDisplay, char *out, size_t outLen)
{
	normalizeIdSegment(labelDisplay, out, outLen);
}

bool
inverterLabelOverrideIsValid(const char *labelOverride)
{
	if (labelOverride == nullptr || labelOverride[0] == '\0') {
		return true;
	}

	for (const unsigned char *cursor = reinterpret_cast<const unsigned char *>(labelOverride); *cursor != '\0'; ++cursor) {
		if (*cursor < 0x20 || *cursor == '"' || *cursor == '\\') {
			return false;
		}
	}

	char labelId[16];
	buildInverterLabelId(labelOverride, labelId, sizeof(labelId));
	return labelId[0] != '\0';
}

bool
buildInverterDeviceDisplayName(const char *serial,
                               const char *labelOverride,
                               char *out,
                               size_t outLen)
{
	if (out == nullptr || outLen == 0) {
		return false;
	}
	out[0] = '\0';
	char label[16];
	if (!buildInverterLabelDisplay(serial, labelOverride, label, sizeof(label))) {
		return false;
	}
	snprintf(out, outLen, "Alpha %s", label);
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

void
buildInverterHaUniqueId(const char *serial, char *out, size_t outLen)
{
	if (out == nullptr || outLen == 0) {
		return;
	}
	if (!inverterSerialIsValid(serial)) {
		snprintf(out, outLen, "%s", "A2M-UNKNOWN");
		return;
	}
	snprintf(out, outLen, "A2M-%s", serial);
}

bool
inverterHaUniqueIdMatchesSerial(const char *uniqueId, const char *serial)
{
	if (uniqueId == nullptr || uniqueId[0] == '\0' || !inverterSerialIsValid(serial)) {
		return false;
	}
	char expected[32];
	buildInverterHaUniqueId(serial, expected, sizeof(expected));
	return strcmp(uniqueId, expected) == 0;
}

bool
buildStaleInverterIdentifier(const char *previousSerial,
                             const char *nextSerial,
                             char *out,
                             size_t outLen)
{
	if (out == nullptr || outLen == 0) {
		return false;
	}
	out[0] = '\0';
	if (!inverterSerialIsValid(previousSerial) ||
	    !inverterSerialIsValid(nextSerial) ||
	    strcmp(previousSerial, nextSerial) == 0) {
		return false;
	}
	buildInverterIdentifier(previousSerial, out, outLen);
	return out[0] != '\0';
}

DiscoveryDeviceScope
mqttEntityScope(mqttEntityId id)
{
	mqttState entity{};
	if (!mqttEntityCopyById(id, &entity)) {
		return DiscoveryDeviceScope::Inverter;
	}
	switch (entity.scope) {
	case MqttEntityScope::Controller:
		return DiscoveryDeviceScope::Controller;
	case MqttEntityScope::Inverter:
	default:
		return DiscoveryDeviceScope::Inverter;
	}
}

void
buildEntityMetricId(const mqttState *entity, char *out, size_t outLen)
{
	if (out == nullptr || outLen == 0) {
		return;
	}
	out[0] = '\0';
	if (entity == nullptr) {
		return;
	}
	if (const MetricNameOverride *entry = findMetricNameOverride(entity->entityId)) {
		snprintf(out, outLen, "%s", entry->metricId);
		return;
	}
	char entityKey[64];
	mqttEntityNameCopy(entity, entityKey, sizeof(entityKey));
	normalizeIdSegment(entityKey, out, outLen);
}

void
buildEntityDisplayName(const mqttState *entity,
                       DiscoveryDeviceScope scope,
                       char *out,
                       size_t outLen)
{
	if (out == nullptr || outLen == 0) {
		return;
	}
	out[0] = '\0';
	if (entity == nullptr) {
		return;
	}
	if (scope == DiscoveryDeviceScope::Inverter) {
		if (const MetricNameOverride *entry = findMetricNameOverride(entity->entityId)) {
			snprintf(out, outLen, "%s", entry->displayName);
			return;
		}
		char entityKey[64];
		mqttEntityNameCopy(entity, entityKey, sizeof(entityKey));
		underscoreNameToDisplay(entityKey, out, outLen);
		return;
	}
	char entityKey[64];
	mqttEntityNameCopy(entity, entityKey, sizeof(entityKey));
	snprintf(out, outLen, "%s", entityKey);
	while (char *ch = strchr(out, '_')) {
		*ch = ' ';
	}
}

void
buildEntityUniqueId(DiscoveryDeviceScope scope,
                    const char *controllerId,
                    const char *serial,
                    const char *metricKey,
                    char *out,
                    size_t outLen)
{
	if (out == nullptr || outLen == 0) {
		return;
	}
	const char *key = metricKey ? metricKey : "";
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
