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
