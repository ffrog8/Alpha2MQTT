// Purpose: Verify RS485 probe backoff and index sequencing stay stable.

#include <doctest/doctest.h>

#include "Rs485ProbeLogic.h"

TEST_CASE("rs485 probe: next backoff doubles and caps")
{
	CHECK(rs485NextBackoffMs(1000, 15000) == 2000);
	CHECK(rs485NextBackoffMs(2000, 15000) == 4000);
	CHECK(rs485NextBackoffMs(4000, 15000) == 8000);
	CHECK(rs485NextBackoffMs(8000, 15000) == 15000);
	CHECK(rs485NextBackoffMs(15000, 15000) == 15000);
	CHECK(rs485NextBackoffMs(30000, 15000) == 15000);
}

TEST_CASE("rs485 probe: next index wraps to zero")
{
	CHECK(rs485NextIndex(-1, 7) == 0);
	CHECK(rs485NextIndex(0, 7) == 1);
	CHECK(rs485NextIndex(5, 7) == 6);
	CHECK(rs485NextIndex(6, 7) == 0);
	CHECK(rs485NextIndex(0, 1) == 0);
}
