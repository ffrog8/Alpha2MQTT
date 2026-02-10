// Purpose: Implement boot/runtime memory evaluation rules for status reporting.
// Responsibilities: threshold comparison and worst-state update.
// Invariants: Pure logic, no Arduino headers, no dynamic allocation.
#include "../include/MemoryHealth.h"

static bool
isWorse(MemLevel candidate, MemLevel current)
{
	return static_cast<uint8_t>(candidate) > static_cast<uint8_t>(current);
}

MemLevel
evaluateBootMem(BootMemStage stage, const MemSample &sample)
{
	if (stage == BootMemStage::Boot0) {
		if (sample.freeB < kBoot0CritFreeB || sample.maxBlockB < kBoot0CritMaxBlockB) {
			return MemLevel::Crit;
		}
		if (sample.freeB < kBoot0WarnFreeB || sample.maxBlockB < kBoot0WarnMaxBlockB) {
			return MemLevel::Warn;
		}
		return MemLevel::Ok;
	}

	if (sample.freeB < kBootNCritFreeB || sample.maxBlockB < kBootNCritMaxBlockB || sample.fragPct > kBootNCritFragPct) {
		return MemLevel::Crit;
	}
	if (sample.freeB < kBootNWarnFreeB || sample.maxBlockB < kBootNWarnMaxBlockB || sample.fragPct > kBootNWarnFragPct) {
		return MemLevel::Warn;
	}
	return MemLevel::Ok;
}

MemLevel
evaluateRuntimeMem(const MemSample &sample)
{
	if (sample.freeB < kRuntimeCritFreeB || sample.maxBlockB < kRuntimeCritMaxBlockB || sample.fragPct > kRuntimeCritFragPct) {
		return MemLevel::Crit;
	}
	if (sample.freeB < kRuntimeWarnFreeB || sample.maxBlockB < kRuntimeWarnMaxBlockB || sample.fragPct > kRuntimeWarnFragPct) {
		return MemLevel::Warn;
	}
	return MemLevel::Ok;
}

bool
updateBootMemWorst(BootMemWorst &worst, BootMemStage stage, const MemSample &sample, MemLevel level)
{
	if (worst.sample.freeB == 0 && worst.sample.maxBlockB == 0 && worst.sample.fragPct == 0 && worst.level == MemLevel::Ok) {
		worst.level = level;
		worst.stage = stage;
		worst.sample = sample;
		return true;
	}
	if (isWorse(level, worst.level)) {
		worst.level = level;
		worst.stage = stage;
		worst.sample = sample;
		return true;
	}
	return false;
}
