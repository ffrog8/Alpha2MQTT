// Purpose: Keep the atomic dispatch request contract small, explicit, and
// host-testable outside the firmware main loop.
// Responsibilities: Parse compact JSON-like MQTT payloads, enforce
// mode-specific validation, translate signed power/SOC/time values into raw
// dispatch registers, and report user-facing mismatch strings.
// Invariants: Parsing is bounded and allocation-free; malformed or out-of-range
// inputs are rejected rather than silently clamped.
#include "../include/DispatchRequest.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

struct DispatchModeSpec {
	DispatchRequestMode mode;
	const char *apiName;
	uint16_t dispatchMode;
	bool usesPower;
	bool usesSoc;
	bool usesDuration;
	bool requireNegativePower;
	bool compareMode;
	bool comparePower;
	bool compareSoc;
	bool compareTime;
};

constexpr DispatchModeSpec kDispatchModeSpecs[] = {
	{ DispatchRequestMode::BatteryOnlyChargesFromPv,
	  "battery_only_charges_from_pv",
	  DISPATCH_MODE_BATTERY_ONLY_CHARGED_VIA_PV,
	  true,
	  false,
	  true,
	  true,
	  true,
	  true,
	  false,
	  true },
	{ DispatchRequestMode::StateOfChargeControl,
	  "state_of_charge_control",
	  DISPATCH_MODE_STATE_OF_CHARGE_CONTROL,
	  true,
	  true,
	  true,
	  false,
	  true,
	  true,
	  true,
	  true },
	{ DispatchRequestMode::LoadFollowing,
	  "load_following",
	  DISPATCH_MODE_LOAD_FOLLOWING,
	  true,
	  false,
	  true,
	  false,
	  true,
	  false,
	  false,
	  true },
	{ DispatchRequestMode::MaximiseOutput,
	  "maximise_output",
	  DISPATCH_MODE_MAXIMISE_OUTPUT,
	  false,
	  false,
	  true,
	  false,
	  true,
	  false,
	  false,
	  true },
	{ DispatchRequestMode::NormalMode,
	  "normal_mode",
	  DISPATCH_MODE_NORMAL_MODE,
	  false,
	  false,
	  false,
	  false,
	  false,
	  false,
	  false,
	  false },
	{ DispatchRequestMode::OptimiseConsumption,
	  "optimise_consumption",
	  DISPATCH_MODE_OPTIMISE_CONSUMPTION,
	  false,
	  false,
	  true,
	  false,
	  true,
	  false,
	  false,
	  true },
	{ DispatchRequestMode::MaximiseConsumption,
	  "maximise_consumption",
	  DISPATCH_MODE_MAXIMISE_CONSUMPTION,
	  false,
	  false,
	  true,
	  false,
	  true,
	  false,
	  false,
	  true },
};

const DispatchModeSpec *
dispatchModeSpecFor(DispatchRequestMode mode)
{
	for (const DispatchModeSpec &spec : kDispatchModeSpecs) {
		if (spec.mode == mode) {
			return &spec;
		}
	}
	return nullptr;
}

static void
copyStatus(char *dest, size_t destSize, const char *message)
{
	if (dest == nullptr || destSize == 0) {
		return;
	}
	if (message == nullptr) {
		dest[0] = '\0';
		return;
	}
	snprintf(dest, destSize, "%s", message);
}

static bool
isTokenChar(char ch)
{
	return (ch >= '0' && ch <= '9') ||
	       (ch >= 'A' && ch <= 'Z') ||
	       (ch >= 'a' && ch <= 'z') ||
	       ch == '_';
}

static const char *
findFieldValue(const char *payload, const char *key)
{
	if (payload == nullptr || key == nullptr) {
		return nullptr;
	}

	const size_t keyLen = strlen(key);
	const char *search = payload;
	while (const char *pos = strstr(search, key)) {
		const char prev = (pos == payload) ? '\0' : pos[-1];
		const char next = pos[keyLen];
		if ((pos == payload || !isTokenChar(prev)) &&
		    (next == '\0' || !isTokenChar(next))) {
			pos += keyLen;
			if (*pos == '"') {
				pos++;
			}
			while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') {
				pos++;
			}
			if (*pos != ':') {
				search = pos;
				continue;
			}
			pos++;
			while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') {
				pos++;
			}
			return pos;
		}
		search = pos + keyLen;
	}
	return nullptr;
}

static bool
extractStringField(const char *payload, const char *key, char *out, size_t outSize)
{
	if (out == nullptr || outSize == 0) {
		return false;
	}
	const char *value = findFieldValue(payload, key);
	if (value == nullptr || *value != '"') {
		return false;
	}
	value++;
	size_t idx = 0;
	while (*value != '\0' && *value != '"' && idx + 1 < outSize) {
		if (*value == '\\' && value[1] != '\0') {
			value++;
		}
		out[idx++] = *value++;
	}
	if (*value != '"') {
		return false;
	}
	out[idx] = '\0';
	return true;
}

static bool
extractInt32Field(const char *payload, const char *key, int32_t &out)
{
	const char *value = findFieldValue(payload, key);
	if (value == nullptr) {
		return false;
	}
	if (*value == '"') {
		value++;
	}
	errno = 0;
	char *endPtr = nullptr;
	const long parsed = strtol(value, &endPtr, 10);
	if (endPtr == value || errno != 0) {
		return false;
	}
	if (*endPtr == '"') {
		endPtr++;
	}
	if (*endPtr != '\0' && *endPtr != ',' && *endPtr != '}' &&
	    *endPtr != ' ' && *endPtr != '\t' && *endPtr != '\r' && *endPtr != '\n') {
		return false;
	}
	out = static_cast<int32_t>(parsed);
	return true;
}

static bool
extractUint32Field(const char *payload, const char *key, uint32_t &out)
{
	const char *value = findFieldValue(payload, key);
	if (value == nullptr) {
		return false;
	}
	if (*value == '"') {
		value++;
	}
	errno = 0;
	char *endPtr = nullptr;
	const unsigned long parsed = strtoul(value, &endPtr, 10);
	if (endPtr == value || errno != 0) {
		return false;
	}
	if (*endPtr == '"') {
		endPtr++;
	}
	if (*endPtr != '\0' && *endPtr != ',' && *endPtr != '}' &&
	    *endPtr != ' ' && *endPtr != '\t' && *endPtr != '\r' && *endPtr != '\n') {
		return false;
	}
	out = static_cast<uint32_t>(parsed);
	return true;
}

static uint16_t
socPercentToRaw(uint16_t socPercent)
{
	return static_cast<uint16_t>(socPercent / DISPATCH_SOC_MULTIPLIER);
}

static int32_t
powerWToRaw(int32_t powerW)
{
	return DISPATCH_ACTIVE_POWER_OFFSET + powerW;
}

} // namespace

const char *
dispatchRequestModeApiName(DispatchRequestMode mode)
{
	const DispatchModeSpec *spec = dispatchModeSpecFor(mode);
	return (spec == nullptr) ? "" : spec->apiName;
}

bool
lookupDispatchRequestMode(const char *name, DispatchRequestMode &mode)
{
	mode = DispatchRequestMode::Unknown;
	if (name == nullptr || name[0] == '\0') {
		return false;
	}
	for (const DispatchModeSpec &spec : kDispatchModeSpecs) {
		if (strcmp(spec.apiName, name) == 0) {
			mode = spec.mode;
			return true;
		}
	}
	return false;
}

bool
parseDispatchRequestPayload(const char *payload,
                            DispatchRequestPayload &out,
                            char *error,
                            size_t errorSize)
{
	out = DispatchRequestPayload{};
	copyStatus(error, errorSize, "");

	if (payload == nullptr || payload[0] == '\0') {
		copyStatus(error, errorSize, "invalid mode");
		return false;
	}

	char modeBuf[48] = { 0 };
	if (!extractStringField(payload, "mode", modeBuf, sizeof(modeBuf)) ||
	    !lookupDispatchRequestMode(modeBuf, out.mode)) {
		copyStatus(error, errorSize, "invalid mode");
		return false;
	}

	int32_t signedValue = 0;
	uint32_t unsignedValue = 0;
	if (extractInt32Field(payload, "power_w", signedValue)) {
		out.hasPower = true;
		out.powerW = signedValue;
	}
	if (extractUint32Field(payload, "soc_percent", unsignedValue)) {
		out.hasSoc = true;
		out.socPercent = static_cast<uint16_t>(unsignedValue);
	}
	if (extractUint32Field(payload, "duration_s", unsignedValue)) {
		out.hasDuration = true;
		out.durationS = unsignedValue;
	}
	return true;
}

bool
buildDispatchRequestPlan(const DispatchRequestPayload &payload,
                         DispatchRequestPlan &out,
                         char *error,
                         size_t errorSize)
{
	out = DispatchRequestPlan{};
	copyStatus(error, errorSize, "");

	const DispatchModeSpec *spec = dispatchModeSpecFor(payload.mode);
	if (spec == nullptr) {
		copyStatus(error, errorSize, "invalid mode");
		return false;
	}

	out.mode = payload.mode;
	out.stop = (payload.mode == DispatchRequestMode::NormalMode);
	out.matchStartStop = out.stop;

	if (spec->usesPower) {
		if (!payload.hasPower || payload.powerW < -INVERTER_POWER_MAX || payload.powerW > INVERTER_POWER_MAX) {
			copyStatus(error, errorSize, "invalid power");
			return false;
		}
		if (spec->requireNegativePower && payload.powerW >= 0) {
			copyStatus(error, errorSize, "invalid power");
			return false;
		}
		out.dispatchActivePower = powerWToRaw(payload.powerW);
	} else {
		out.dispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET;
	}

	if (spec->usesSoc) {
		if (!payload.hasSoc || payload.socPercent > 100U) {
			copyStatus(error, errorSize, "invalid soc");
			return false;
		}
		out.dispatchSocRaw = socPercentToRaw(payload.socPercent);
	}

	if (spec->usesDuration) {
		if (!payload.hasDuration || payload.durationS > kDispatchDurationMaxSeconds) {
			copyStatus(error, errorSize, "invalid duration");
			return false;
		}
		out.dispatchTimeRaw = dispatchRawTimeForDuration(payload.durationS);
	}

	if (!out.stop) {
		out.dispatchMode = spec->dispatchMode;
		out.matchMode = spec->compareMode;
		out.matchPower = spec->comparePower;
		out.matchSoc = spec->compareSoc;
		out.matchTime = spec->compareTime;
	}

	return true;
}

bool
dispatchRequestReadbackMatches(const DispatchRequestPlan &plan,
                               const DispatchRegisterReadback &readback,
                               char *error,
                               size_t errorSize)
{
	copyStatus(error, errorSize, "");

	if (plan.matchStartStop) {
		if (readback.dispatchStart != DISPATCH_START_STOP) {
			copyStatus(error, errorSize, "dispatch start mismatch");
			return false;
		}
		return true;
	}

	if (readback.dispatchStart != DISPATCH_START_START) {
		copyStatus(error, errorSize, "dispatch start mismatch");
		return false;
	}
	if (plan.matchMode && readback.dispatchMode != plan.dispatchMode) {
		copyStatus(error, errorSize, "dispatch mode mismatch");
		return false;
	}
	if (plan.matchPower && readback.dispatchActivePower != plan.dispatchActivePower) {
		copyStatus(error, errorSize, "dispatch power mismatch");
		return false;
	}
	if (plan.matchSoc && readback.dispatchSocRaw != plan.dispatchSocRaw) {
		copyStatus(error, errorSize, "dispatch soc mismatch");
		return false;
	}
	if (plan.matchTime && readback.dispatchTimeRaw != plan.dispatchTimeRaw) {
		copyStatus(error, errorSize, "dispatch time mismatch");
		return false;
	}
	return true;
}
