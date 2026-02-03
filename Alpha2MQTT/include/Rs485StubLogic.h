/*
  Rs485StubLogic.h

  Pure helper logic for the RS485/Modbus stub backend (no Arduino deps).
*/
#pragma once

#include <cstdint>

enum class Rs485StubMode : uint8_t {
	OfflineForever = 0,
	OnlineAlways = 1,
	FailFirstNThenRecover = 2,
};

struct Rs485StubConfig {
	Rs485StubMode mode = Rs485StubMode::OfflineForever;
	uint32_t failFirstN = 0;
	// When non-zero, fail reads that touch this starting register.
	uint16_t failRegister = 0;
};

static inline bool
rs485StubShouldFail(const Rs485StubConfig &cfg, uint32_t attemptIndexOneBased, uint16_t startRegister)
{
	if (cfg.failRegister != 0 && startRegister == cfg.failRegister) {
		return true;
	}

	switch (cfg.mode) {
	case Rs485StubMode::OfflineForever:
		return true;
	case Rs485StubMode::OnlineAlways:
		return false;
	case Rs485StubMode::FailFirstNThenRecover:
		return attemptIndexOneBased <= cfg.failFirstN;
	default:
		return true;
	}
}

static inline uint16_t
rs485StubWordForRegister(uint16_t reg)
{
	// Deterministic pseudo data: stable across boots and builds.
	return static_cast<uint16_t>(reg ^ 0xA55A);
}

