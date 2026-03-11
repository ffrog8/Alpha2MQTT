#include "../include/Scheduler.h"

bool shouldRun(uint32_t now, uint32_t lastRun, uint32_t intervalMs)
{
	if (lastRun == 0) {
		return true;
	}
	return static_cast<uint32_t>(now - lastRun) >= intervalMs;
}

bool timedOut(uint32_t start, uint32_t now, uint32_t limitMs)
{
	return static_cast<uint32_t>(now - start) >= limitMs;
}

uint32_t resetScheduleBaseline(uint32_t now)
{
	return now;
}

size_t normalizeDeferredCursor(size_t cursor, size_t totalCount)
{
	if (totalCount == 0) {
		return 0;
	}
	return cursor % totalCount;
}

size_t nextDeferredCursor(size_t startCursor, size_t processedCount, size_t totalCount, bool truncated)
{
	if (totalCount == 0) {
		return 0;
	}
	if (!truncated || processedCount >= totalCount) {
		return 0;
	}
	return normalizeDeferredCursor(startCursor + processedCount, totalCount);
}

void updateBucketRuntimeBudgetState(BucketRuntimeBudgetState &state,
                                    uint32_t nowMs,
                                    uint32_t elapsedMs,
                                    uint32_t budgetMs,
                                    size_t totalCount,
                                    size_t processedCount,
                                    bool truncated)
{
	state.observed = true;
	state.usedMsLast = elapsedMs;
	state.limitMsLast = budgetMs;
	state.backlogCount = (truncated && processedCount < totalCount)
		? static_cast<uint16_t>(totalCount - processedCount)
		: 0;

	if (state.backlogCount == 0) {
		state.deferredSinceMs = 0;
		state.lastFullCycleCompletedMs = nowMs;
	} else if (state.deferredSinceMs == 0) {
		state.deferredSinceMs = nowMs;
	}
}

bool bucketRuntimeBudgetExceeded(const BucketRuntimeBudgetState &state)
{
	return state.backlogCount != 0;
}

uint32_t bucketBacklogOldestAgeMs(const BucketRuntimeBudgetState &state, uint32_t nowMs)
{
	if (!bucketRuntimeBudgetExceeded(state) || state.deferredSinceMs == 0) {
		return 0;
	}
	return static_cast<uint32_t>(nowMs - state.deferredSinceMs);
}

uint32_t bucketLastFullCycleAgeMs(const BucketRuntimeBudgetState &state, uint32_t nowMs)
{
	if (!state.observed) {
		return 0;
	}
	if (state.lastFullCycleCompletedMs == 0) {
		return nowMs;
	}
	return static_cast<uint32_t>(nowMs - state.lastFullCycleCompletedMs);
}
