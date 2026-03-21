#pragma once

#include <cstdint>

static inline uint32_t rs485NextBackoffMs(uint32_t currentMs, uint32_t maxMs)
{
	if (currentMs == 0) {
		return 1;
	}
	if (currentMs >= maxMs) {
		return maxMs;
	}
	uint32_t next = currentMs * 2;
	if (next < currentMs || next > maxMs) {
		return maxMs;
	}
	return next;
}

static inline int rs485NextIndex(int current, int count)
{
	if (count <= 0) {
		return 0;
	}
	int next = current + 1;
	if (next >= count) {
		return 0;
	}
	if (next < 0) {
		return 0;
	}
	return next;
}
