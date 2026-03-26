// Purpose: Keep timed-dispatch bookkeeping small, explicit, and host-testable.
// Responsibilities: Clamp configured duration values, track restartable timed
// generations, and provide controller-derived remaining-time helpers without
// conflating them with the inverter's raw Dispatch_Time register.
#pragma once

#include <cstdint>

constexpr uint32_t kDispatchDurationForeverSeconds = 0;
constexpr uint32_t kDispatchRawForeverSeconds = 0x7FFFFFFFUL;
// The controller countdown is derived from uint32_t millis(), so timed runs
// must stay below one full wrap period to keep elapsed-time math monotonic.
constexpr uint32_t kDispatchDurationMaxSeconds = 0xFFFFFFFFUL / 1000UL;
constexpr uint32_t kDispatchCountdownPublishIntervalMs = 5000UL;
constexpr uint32_t kDispatchHandshakeIntervalMs = 1000UL;

struct TimedDispatchRuntimeState {
	uint32_t configuredDurationSeconds = kDispatchDurationForeverSeconds;
	uint32_t requestedGeneration = 0;
	uint32_t activeGeneration = 0;
	uint32_t completedGeneration = 0;
	uint32_t acceptedAtMs = 0;
	uint32_t acceptedDurationSeconds = 0;
	uint32_t lastCountdownPublishMs = 0;
	uint32_t lastEvalMs = 0;
	bool evalPending = false;
	bool awaitingStopAck = false;
	bool restartAfterStop = false;
	bool bootStopPending = true;
};

uint32_t clampDispatchDurationSeconds(uint32_t seconds);
bool dispatchDurationIsTimed(uint32_t seconds);
uint32_t dispatchRawTimeForDuration(uint32_t seconds);
uint32_t dispatchRemainingSeconds(uint32_t acceptedAtMs,
                                  uint32_t acceptedDurationSeconds,
                                  uint32_t nowMs);
bool dispatchEvalDue(uint32_t lastEvalMs, uint32_t nowMs, uint32_t intervalMs, bool forceImmediate);
bool dispatchCountdownPublishDue(uint32_t lastCountdownPublishMs, uint32_t nowMs);
bool dispatchUseFastEvalCadence(const TimedDispatchRuntimeState &state,
                                bool timedEnabled,
                                bool rs485Live);
void dispatchNoteRequestedGeneration(TimedDispatchRuntimeState &state);
bool dispatchHasPendingGeneration(const TimedDispatchRuntimeState &state);
void dispatchMarkAccepted(TimedDispatchRuntimeState &state,
                          uint32_t generation,
                          uint32_t acceptedAtMs,
                          uint32_t durationSeconds);
void dispatchMarkStopped(TimedDispatchRuntimeState &state, bool completed);
