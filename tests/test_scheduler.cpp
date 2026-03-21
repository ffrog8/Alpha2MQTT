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

TEST_CASE("scheduler deferred cursor advances from the first skipped transaction")
{
	CHECK(normalizeDeferredCursor(5, 4) == 1);
	CHECK(normalizeDeferredCursor(0, 0) == 0);
	CHECK(nextDeferredCursor(2, 3, 7, true) == 5);
	CHECK(nextDeferredCursor(6, 2, 7, true) == 1);
}

TEST_CASE("scheduler deferred cursor resets after a full bucket pass")
{
	CHECK(nextDeferredCursor(0, 4, 4, false) == 0);
	CHECK(nextDeferredCursor(3, 4, 4, true) == 0);
	CHECK(nextDeferredCursor(1, 0, 4, true) == 1);
}

TEST_CASE("scheduler runtime budget state tracks deferred backlog until a full cycle completes")
{
	BucketRuntimeBudgetState state{};

	updateBucketRuntimeBudgetState(state, 1000u, 450u, 400u, 5u, 3u, true);
	CHECK(state.observed);
	CHECK(bucketRuntimeBudgetExceeded(state));
	CHECK(state.backlogCount == 2);
	CHECK(state.usedMsLast == 450u);
	CHECK(state.limitMsLast == 400u);
	CHECK(bucketBacklogOldestAgeMs(state, 1600u) == 600u);
	CHECK(bucketLastFullCycleAgeMs(state, 1600u) == 1600u);

	updateBucketRuntimeBudgetState(state, 1500u, 420u, 400u, 5u, 4u, true);
	CHECK(state.backlogCount == 1);
	CHECK(bucketBacklogOldestAgeMs(state, 2000u) == 1000u);

	updateBucketRuntimeBudgetState(state, 3000u, 250u, 400u, 5u, 5u, false);
	CHECK_FALSE(bucketRuntimeBudgetExceeded(state));
	CHECK(state.backlogCount == 0);
	CHECK(bucketBacklogOldestAgeMs(state, 3200u) == 0u);
	CHECK(bucketLastFullCycleAgeMs(state, 3200u) == 200u);
}
