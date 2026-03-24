// Purpose: Implement timed-dispatch bookkeeping with no Arduino dependencies.
#include "../include/DispatchTiming.h"

uint32_t
clampDispatchDurationSeconds(uint32_t seconds)
{
	if (seconds > kDispatchDurationMaxSeconds) {
		return kDispatchDurationMaxSeconds;
	}
	return seconds;
}

bool
dispatchDurationIsTimed(uint32_t seconds)
{
	return clampDispatchDurationSeconds(seconds) > kDispatchDurationForeverSeconds;
}

uint32_t
dispatchRawTimeForDuration(uint32_t seconds)
{
	if (!dispatchDurationIsTimed(seconds)) {
		return kDispatchRawForeverSeconds;
	}
	return clampDispatchDurationSeconds(seconds);
}

uint32_t
dispatchRemainingSeconds(uint32_t acceptedAtMs, uint32_t acceptedDurationSeconds, uint32_t nowMs)
{
	if (!dispatchDurationIsTimed(acceptedDurationSeconds)) {
		return 0;
	}
	const uint32_t elapsedMs = nowMs - acceptedAtMs;
	const uint32_t elapsedSeconds = elapsedMs / 1000UL;
	if (elapsedSeconds >= acceptedDurationSeconds) {
		return 0;
	}
	return acceptedDurationSeconds - elapsedSeconds;
}

bool
dispatchEvalDue(uint32_t lastEvalMs, uint32_t nowMs, uint32_t intervalMs, bool forceImmediate)
{
	if (forceImmediate) {
		return true;
	}
	if (intervalMs == 0) {
		return true;
	}
	return static_cast<uint32_t>(nowMs - lastEvalMs) >= intervalMs;
}

bool
dispatchCountdownPublishDue(uint32_t lastCountdownPublishMs, uint32_t nowMs)
{
	return static_cast<uint32_t>(nowMs - lastCountdownPublishMs) >= kDispatchCountdownPublishIntervalMs;
}

void
dispatchNoteRequestedGeneration(TimedDispatchRuntimeState &state)
{
	if (state.activeGeneration == 0 &&
	    state.requestedGeneration != 0 &&
	    state.requestedGeneration > state.completedGeneration) {
		return;
	}
	state.requestedGeneration++;
	if (state.requestedGeneration == 0) {
		state.requestedGeneration = 1;
	}
}

bool
dispatchHasPendingGeneration(const TimedDispatchRuntimeState &state)
{
	return state.requestedGeneration > state.completedGeneration;
}

void
dispatchMarkAccepted(TimedDispatchRuntimeState &state,
                     uint32_t generation,
                     uint32_t acceptedAtMs,
                     uint32_t durationSeconds)
{
	state.activeGeneration = generation;
	state.acceptedAtMs = acceptedAtMs;
	state.acceptedDurationSeconds = durationSeconds;
	state.lastCountdownPublishMs = acceptedAtMs;
	state.evalPending = false;
	state.awaitingStopAck = false;
	state.restartAfterStop = false;
}

void
dispatchMarkStopped(TimedDispatchRuntimeState &state, bool completed)
{
	if (completed && state.activeGeneration != 0) {
		state.completedGeneration = state.activeGeneration;
	}
	state.activeGeneration = 0;
	state.acceptedAtMs = 0;
	state.acceptedDurationSeconds = 0;
	state.awaitingStopAck = false;
	state.restartAfterStop = false;
	state.lastCountdownPublishMs = 0;
	state.evalPending = false;
}
