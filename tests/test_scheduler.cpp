#include "doctest/doctest.h"

#include "Scheduler.h"

TEST_CASE("scheduler shouldRun handles wraparound")
{
	uint32_t last = 0xFFFFFFF0u;
	uint32_t now = 0x00000010u;
	CHECK(shouldRun(now, last, 0x20u));
	CHECK_FALSE(shouldRun(now, last, 0x30u));
}

TEST_CASE("scheduler shouldRun baseline behavior")
{
	uint32_t now = 1000u;
	CHECK(shouldRun(now, 0u, 500u));
	CHECK_FALSE(shouldRun(now + 100u, now, 500u));
	CHECK(shouldRun(now + 500u, now, 500u));
}

TEST_CASE("scheduler pause and resume resets baseline")
{
	uint32_t last = 100u;
	uint32_t interval = 50u;
	CHECK_FALSE(shouldRun(140u, last, interval));

	last = resetScheduleBaseline(500u);
	CHECK_FALSE(shouldRun(520u, last, interval));
	CHECK(shouldRun(550u, last, interval));
}

TEST_CASE("scheduler timedOut uses wraparound")
{
	uint32_t start = 0xFFFFFF00u;
	uint32_t now = 0x00000020u;
	CHECK(timedOut(start, now, 0x30u));
	CHECK_FALSE(timedOut(start, now, 0x200u));
}
