#include "doctest/doctest.h"

#include "ModbusCodec.h"

TEST_CASE("modbus CRC matches known vector")
{
	uint8_t frame[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00 };
	appendCrc(frame, sizeof(frame));
	CHECK(frame[6] == 0xC5);
	CHECK(frame[7] == 0xCD);
}

TEST_CASE("modbus checkCRC validates frames")
{
	uint8_t frame[] = { 0x01, 0x03, 0x00, 0x00, 0x00, 0x0A, 0x00, 0x00 };
	appendCrc(frame, sizeof(frame));
	CHECK(checkCrc(frame, sizeof(frame)));

	frame[1] = 0x04;
	CHECK_FALSE(checkCrc(frame, sizeof(frame)));
}
