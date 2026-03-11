#pragma once

#include <cstddef>
#include <cstdint>

#include "Definitions.h"

// Scheduling helpers are wraparound-safe and treat a baseline of 0 as "run now".
// When resuming after a pause, reset the baseline to now to avoid catch-up floods.

bool shouldRun(uint32_t now, uint32_t lastRun, uint32_t intervalMs);
bool timedOut(uint32_t start, uint32_t now, uint32_t limitMs);
uint32_t resetScheduleBaseline(uint32_t now);
size_t normalizeDeferredCursor(size_t cursor, size_t totalCount);
size_t nextDeferredCursor(size_t startCursor, size_t processedCount, size_t totalCount, bool truncated);
