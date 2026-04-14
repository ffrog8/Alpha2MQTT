// Purpose: Parse the raw debug read MQTT request payload without Arduino deps.
// Responsibilities: Find top-level JSON keys, parse decimal and hex register
// fields, and enforce the raw-read byte limits used by the firmware handler.
// Invariants: Keys embedded in nested objects or string values are ignored so
// unrelated metadata cannot redirect or invalidate a raw Modbus debug read.

#include "../include/RawReadRequest.h"

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>

namespace {

static bool
isRawReadFieldTerminator(char ch)
{
	return ch == '\0' || ch == ',' || ch == '}' || ch == ']' ||
	       std::isspace(static_cast<unsigned char>(ch));
}

static const char *
findTopLevelFieldValue(const char *payload, const char *key)
{
	if (payload == nullptr || key == nullptr) {
		return nullptr;
	}

	const size_t keyLen = strlen(key);
	int depth = 0;
	bool inString = false;
	bool escape = false;
	bool candidateKey = false;
	bool sawEscapeInString = false;
	const char *stringStart = nullptr;
	for (const char *pos = payload; *pos != '\0'; pos++) {
		const char ch = *pos;
		if (inString) {
			if (escape) {
				escape = false;
				continue;
			}
			if (ch == '\\') {
				escape = true;
				sawEscapeInString = true;
				continue;
			}
			if (ch != '"') {
				continue;
			}

			inString = false;
			if (!(candidateKey && !sawEscapeInString && stringStart != nullptr)) {
				continue;
			}
			if (static_cast<size_t>(pos - stringStart) != keyLen ||
			    strncmp(stringStart, key, keyLen) != 0) {
				continue;
			}

			const char *value = pos + 1;
			while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
				value++;
			}
			if (*value != ':') {
				continue;
			}
			value++;
			while (*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') {
				value++;
			}
			return value;
		}

		if (ch == '{' || ch == '[') {
			depth++;
			continue;
		}
		if (ch == '}' || ch == ']') {
			if (depth > 0) {
				depth--;
			}
			continue;
		}
		if (ch != '"') {
			continue;
		}

		const char *before = pos;
		while (before > payload) {
			const char prev = before[-1];
			if (prev == ' ' || prev == '\t' || prev == '\r' || prev == '\n') {
				before--;
				continue;
			}
			break;
		}
		candidateKey = (depth == 1 && before > payload && (before[-1] == '{' || before[-1] == ','));
		sawEscapeInString = false;
		escape = false;
		inString = true;
		stringStart = pos + 1;
	}
	return nullptr;
}

static bool
parseRawReadDecimalField(const char *payload, const char *key, int32_t &out)
{
	const char *value = findTopLevelFieldValue(payload, key);
	if (value == nullptr) {
		return false;
	}

	const bool quoted = (*value == '"');
	if (quoted) {
		value++;
	}

	errno = 0;
	char *endPtr = nullptr;
	const long parsed = strtol(value, &endPtr, 10);
	if (errno != 0 || endPtr == value || parsed < INT32_MIN || parsed > INT32_MAX) {
		return false;
	}
	if (quoted) {
		if (*endPtr != '"') {
			return false;
		}
		endPtr++;
	}
	if (!isRawReadFieldTerminator(*endPtr)) {
		return false;
	}
	out = static_cast<int32_t>(parsed);
	return true;
}

static bool
parseRawReadHexField(const char *payload, const char *key, uint16_t &out)
{
	const char *value = findTopLevelFieldValue(payload, key);
	if (value == nullptr) {
		return false;
	}

	const bool quoted = (*value == '"');
	if (quoted) {
		value++;
	}
	if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) {
		value += 2;
	}
	if (!std::isxdigit(static_cast<unsigned char>(*value))) {
		return false;
	}

	errno = 0;
	char *endPtr = nullptr;
	const unsigned long parsed = strtoul(value, &endPtr, 16);
	if (errno != 0 || endPtr == value || parsed > UINT16_MAX) {
		return false;
	}
	if (quoted) {
		if (*endPtr != '"') {
			return false;
		}
		endPtr++;
	}
	if (!isRawReadFieldTerminator(*endPtr)) {
		return false;
	}
	out = static_cast<uint16_t>(parsed);
	return true;
}

} // namespace

bool
parseRawReadRequestPayload(const char *payload, RawReadRequest &request)
{
	request = RawReadRequest{};
	int32_t decimalValue = 0;
	uint16_t hexValue = 0;

	if (parseRawReadDecimalField(payload, "register", decimalValue)) {
		request.requestedReg = decimalValue;
		request.hasRegister = true;
	} else if (parseRawReadHexField(payload, "registerAddress", hexValue)) {
		request.requestedReg = hexValue;
		request.hasRegister = true;
	}

	if (parseRawReadDecimalField(payload, "bytes", decimalValue)) {
		request.requestedBytes = decimalValue;
		request.hasBytes = true;
	} else if (parseRawReadDecimalField(payload, "dataBytes", decimalValue)) {
		request.requestedBytes = decimalValue;
		request.hasBytes = true;
	}

	if (!request.hasRegister || !request.hasBytes) {
		return false;
	}
	if (request.requestedReg < 0 || request.requestedReg > UINT16_MAX) {
		return false;
	}
	if (request.requestedBytes < 2 || request.requestedBytes > kRawReadMaxBytes) {
		return false;
	}
	if ((request.requestedBytes & 1) != 0) {
		return false;
	}
	return true;
}
