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
