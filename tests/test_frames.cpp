#include "doctest/doctest.h"

#include <limits>
#include <vector>

#include "ModbusCodec.h"

namespace {
constexpr uint8_t kReadFunction = 0x03;
constexpr uint8_t kWriteMultipleFunction = 0x10;
}

TEST_CASE("modbus read frame layout and CRC")
{
	uint8_t frame[kModbusReadFrameSize];
	buildReadFrame(0x01, kReadFunction, 0x0000, 0x000A, frame);
	appendCrc(frame, sizeof(frame));

	CHECK(frame[0] == 0x01);
	CHECK(frame[1] == 0x03);
	CHECK(frame[2] == 0x00);
	CHECK(frame[3] == 0x00);
	CHECK(frame[4] == 0x00);
	CHECK(frame[5] == 0x0A);
	CHECK(frame[6] == 0xC5);
	CHECK(frame[7] == 0xCD);
}

TEST_CASE("modbus write multiple registers frame layout and CRC")
{
	uint16_t values[] = { 0x000A, 0x0102 };
	size_t size = writeMultipleRegistersFrameSize(2);
	std::vector<uint8_t> frame(size, 0);

	size_t written = buildWriteMultipleRegistersFrame(0x01,
					 kWriteMultipleFunction,
					 0x0001,
					 values,
					 2,
					 frame.data(),
					 frame.size());

	REQUIRE(written == size);
	CHECK(frame[0] == 0x01);
	CHECK(frame[1] == 0x10);
	CHECK(frame[2] == 0x00);
	CHECK(frame[3] == 0x01);
	CHECK(frame[4] == 0x00);
	CHECK(frame[5] == 0x02);
	CHECK(frame[6] == 0x04);
	CHECK(frame[7] == 0x00);
	CHECK(frame[8] == 0x0A);
	CHECK(frame[9] == 0x01);
	CHECK(frame[10] == 0x02);
	CHECK(frame[11] == 0x92);
	CHECK(frame[12] == 0x30);
}

TEST_CASE("modbus frames handle boundary register values")
{
	uint8_t readFrame[kModbusReadFrameSize];
	buildReadFrame(0x0A, kReadFunction, 0x0000, 0x0001, readFrame);
	CHECK(readFrame[2] == 0x00);
	CHECK(readFrame[3] == 0x00);
	CHECK(readFrame[4] == 0x00);
	CHECK(readFrame[5] == 0x01);

	buildReadFrame(0x0A, kReadFunction, 0xFFFF, 0xFFFF, readFrame);
	CHECK(readFrame[2] == 0xFF);
	CHECK(readFrame[3] == 0xFF);
	CHECK(readFrame[4] == 0xFF);
	CHECK(readFrame[5] == 0xFF);

	CHECK(writeMultipleRegistersFrameSize(1) == 11u);
	CHECK(writeMultipleRegistersFrameSize(std::numeric_limits<uint16_t>::max()) == 131079u);
}
