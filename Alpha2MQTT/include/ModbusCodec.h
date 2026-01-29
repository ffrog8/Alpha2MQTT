#pragma once

#include <cstddef>
#include <cstdint>

constexpr size_t kModbusReadFrameSize = 8;

uint16_t calculateCrc(const uint8_t *frame, size_t length);
void appendCrc(uint8_t *frame, size_t frameSize);
bool checkCrc(const uint8_t *frame, size_t frameSize);

void buildReadFrame(uint8_t slaveAddress,
			    uint8_t functionCode,
			    uint16_t registerAddress,
			    uint16_t registerCount,
			    uint8_t frame[kModbusReadFrameSize]);

size_t writeMultipleRegistersFrameSize(uint16_t registerCount);
size_t buildWriteMultipleRegistersFrame(uint8_t slaveAddress,
				       uint8_t functionCode,
				       uint16_t registerAddress,
				       const uint16_t *values,
				       uint16_t registerCount,
				       uint8_t *frame,
				       size_t frameSize);
