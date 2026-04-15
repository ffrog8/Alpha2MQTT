// Purpose: Define the raw debug read MQTT request contract and parser.
// Responsibilities: Parse the flat JSON payload used by debug/raw_read/set,
// validate register and byte limits, and keep parsing independent of Arduino.
// Invariants: Only top-level JSON keys are matched, odd byte counts are
// rejected, and accepted registers always fit in a Modbus uint16_t address.
#pragma once

#include <cstdint>

#include "Definitions.h"

struct RawReadRequest {
	int32_t requestedReg = -1;
	int32_t requestedBytes = 0;
	bool hasRegister = false;
	bool hasBytes = false;
};

constexpr uint16_t kRawReadResponseOverheadBytes = 6;
constexpr uint16_t kRawReadMaxBytes = MAX_FRAME_SIZE - kRawReadResponseOverheadBytes;

bool parseRawReadRequestPayload(const char *payload, RawReadRequest &request);
