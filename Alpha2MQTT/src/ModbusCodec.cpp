#include "../include/ModbusCodec.h"

uint16_t calculateCrc(const uint8_t *frame, size_t length)
{
	unsigned int temp = 0xffff;

	for (size_t i = 0; i < length; i++) {
		temp = temp ^ frame[i];

		for (unsigned char j = 1; j <= 8; j++) {
			unsigned int flag = temp & 0x0001;
			temp >>= 1;

			if (flag) {
				temp ^= 0xA001;
			}
		}
	}

	return static_cast<uint16_t>(temp);
}

void appendCrc(uint8_t *frame, size_t frameSize)
{
	if (frameSize < 2) {
		return;
	}
	uint16_t crc = calculateCrc(frame, frameSize - 2);
	frame[frameSize - 2] = static_cast<uint8_t>(crc & 0xff);
	frame[frameSize - 1] = static_cast<uint8_t>(crc >> 8);
}

bool checkCrc(const uint8_t *frame, size_t frameSize)
{
	if (frameSize < 2) {
		return false;
	}
	uint16_t received = static_cast<uint16_t>((frame[frameSize - 2] << 8) | frame[frameSize - 1]);
	uint16_t calculated = calculateCrc(frame, frameSize - 2);
	uint16_t calculatedReversed = static_cast<uint16_t>((calculated & 0xff) << 8 | (calculated >> 8));
	return received == calculatedReversed;
}

void buildReadFrame(uint8_t slaveAddress,
			    uint8_t functionCode,
			    uint16_t registerAddress,
			    uint16_t registerCount,
			    uint8_t frame[kModbusReadFrameSize])
{
	frame[0] = slaveAddress;
	frame[1] = functionCode;
	frame[2] = static_cast<uint8_t>((registerAddress >> 8) & 0xff);
	frame[3] = static_cast<uint8_t>(registerAddress & 0xff);
	frame[4] = static_cast<uint8_t>((registerCount >> 8) & 0xff);
	frame[5] = static_cast<uint8_t>(registerCount & 0xff);
	frame[6] = 0;
	frame[7] = 0;
}

size_t writeMultipleRegistersFrameSize(uint16_t registerCount)
{
	return static_cast<size_t>(9U + static_cast<size_t>(registerCount) * 2U);
}

size_t buildWriteMultipleRegistersFrame(uint8_t slaveAddress,
				       uint8_t functionCode,
				       uint16_t registerAddress,
				       const uint16_t *values,
				       uint16_t registerCount,
				       uint8_t *frame,
				       size_t frameSize)
{
	size_t required = writeMultipleRegistersFrameSize(registerCount);
	if (frameSize < required) {
		return 0;
	}
	frame[0] = slaveAddress;
	frame[1] = functionCode;
	frame[2] = static_cast<uint8_t>((registerAddress >> 8) & 0xff);
	frame[3] = static_cast<uint8_t>(registerAddress & 0xff);
	frame[4] = static_cast<uint8_t>((registerCount >> 8) & 0xff);
	frame[5] = static_cast<uint8_t>(registerCount & 0xff);
	frame[6] = static_cast<uint8_t>(registerCount * 2);

	size_t offset = 7;
	for (uint16_t i = 0; i < registerCount; i++) {
		frame[offset++] = static_cast<uint8_t>((values[i] >> 8) & 0xff);
		frame[offset++] = static_cast<uint8_t>(values[i] & 0xff);
	}

	frame[offset++] = 0;
	frame[offset++] = 0;
	appendCrc(frame, required);
	return required;
}
