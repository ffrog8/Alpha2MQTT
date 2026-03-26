#include <doctest/doctest.h>

#include "DispatchTiming.h"

TEST_CASE("dispatch timing clamps and preserves forever semantics")
{
	CHECK(clampDispatchDurationSeconds(0) == 0);
	CHECK(clampDispatchDurationSeconds(12) == 12);
	CHECK(kDispatchDurationMaxSeconds == 4294967UL);
	CHECK(clampDispatchDurationSeconds(0xFFFFFFFFUL) == kDispatchDurationMaxSeconds);
	CHECK_FALSE(dispatchDurationIsTimed(0));
	CHECK(dispatchDurationIsTimed(5));
	CHECK(dispatchRawTimeForDuration(0) == kDispatchRawForeverSeconds);
	CHECK(dispatchRawTimeForDuration(120) == 120);
}

TEST_CASE("dispatch timing max clamp remains millis-safe")
{
	CHECK(clampDispatchDurationSeconds(kDispatchDurationMaxSeconds) == kDispatchDurationMaxSeconds);
	CHECK(clampDispatchDurationSeconds(kDispatchDurationMaxSeconds + 1UL) == kDispatchDurationMaxSeconds);
}

TEST_CASE("dispatch timing remaining countdown floors at zero")
{
	CHECK(dispatchRemainingSeconds(1000, 0, 6000) == 0);
	CHECK(dispatchRemainingSeconds(1000, 15, 1000) == 15);
	CHECK(dispatchRemainingSeconds(1000, 15, 7000) == 9);
	CHECK(dispatchRemainingSeconds(1000, 15, 17000) == 0);
}

TEST_CASE("dispatch timing generation bookkeeping avoids accidental restart")
{
	TimedDispatchRuntimeState state{};
	CHECK_FALSE(dispatchHasPendingGeneration(state));

	dispatchNoteRequestedGeneration(state);
	CHECK(state.requestedGeneration == 1);
	CHECK(dispatchHasPendingGeneration(state));

	dispatchNoteRequestedGeneration(state);
	CHECK(state.requestedGeneration == 1);
	CHECK(dispatchHasPendingGeneration(state));

	dispatchMarkAccepted(state, state.requestedGeneration, 5000, 120);
	CHECK(state.activeGeneration == 1);
	CHECK(state.acceptedDurationSeconds == 120);
	CHECK(state.completedGeneration == 0);

	dispatchMarkStopped(state, true);
	CHECK(state.activeGeneration == 0);
	CHECK(state.completedGeneration == 1);
	CHECK_FALSE(dispatchHasPendingGeneration(state));

	dispatchNoteRequestedGeneration(state);
	CHECK(state.requestedGeneration == 2);
	CHECK(dispatchHasPendingGeneration(state));
}

TEST_CASE("dispatch timing stop never regresses completed generation")
{
	TimedDispatchRuntimeState state{};
	state.activeGeneration = 2;
	state.completedGeneration = 5;

	dispatchMarkStopped(state, true);

	CHECK(state.activeGeneration == 0);
	CHECK(state.completedGeneration == 5);
}

TEST_CASE("dispatch timing evaluation and countdown cadences are independent")
{
	CHECK_FALSE(dispatchEvalDue(1000, 3500, 3000, false));
	CHECK(dispatchEvalDue(1000, 4000, 3000, false));
	CHECK(dispatchEvalDue(1000, 3500, 0, false));
	CHECK(dispatchEvalDue(0, 500, 120000, true));

	CHECK_FALSE(dispatchCountdownPublishDue(1000, 5999));
	CHECK(dispatchCountdownPublishDue(1000, 6000));
}
