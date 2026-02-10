// Memory health policy helpers for boot/runtime monitoring.
// Purpose: centralize thresholds + evaluation rules with no Arduino deps.
// Invariants: No dynamic allocation; deterministic evaluation for host tests.
#pragma once

#include <cstdint>

enum class MemLevel : uint8_t {
	Ok = 0,
	Warn = 1,
	Crit = 2
};

enum class BootMemStage : uint8_t {
	Boot0 = 0,
	Boot1 = 1,
	Boot2 = 2,
	Boot3 = 3,
	Boot4 = 4
};

struct MemSample {
	uint32_t freeB;
	uint32_t maxBlockB;
	uint8_t fragPct;
};

struct BootMemWorst {
	MemLevel level;
	BootMemStage stage;
	MemSample sample;
};

constexpr uint32_t kBoot0WarnFreeB = 14000;
constexpr uint32_t kBoot0WarnMaxBlockB = 14000;
constexpr uint32_t kBoot0CritFreeB = 12000;
constexpr uint32_t kBoot0CritMaxBlockB = 12000;

constexpr uint32_t kBootNWarnFreeB = 6000;
constexpr uint32_t kBootNWarnMaxBlockB = 4096;
constexpr uint8_t kBootNWarnFragPct = 25;
constexpr uint32_t kBootNCritFreeB = 4000;
constexpr uint32_t kBootNCritMaxBlockB = 2048;
constexpr uint8_t kBootNCritFragPct = 35;

constexpr uint32_t kRuntimeWarnFreeB = 6144;
constexpr uint32_t kRuntimeWarnMaxBlockB = 4096;
constexpr uint8_t kRuntimeWarnFragPct = 25;
constexpr uint32_t kRuntimeCritFreeB = 4096;
constexpr uint32_t kRuntimeCritMaxBlockB = 2048;
constexpr uint8_t kRuntimeCritFragPct = 35;

MemLevel evaluateBootMem(BootMemStage stage, const MemSample &sample);
MemLevel evaluateRuntimeMem(const MemSample &sample);
bool updateBootMemWorst(BootMemWorst &worst, BootMemStage stage, const MemSample &sample, MemLevel level);
