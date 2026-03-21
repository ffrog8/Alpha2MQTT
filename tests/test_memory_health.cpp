// Purpose: Validate boot/runtime memory policy logic and worst-state latch.
#include "doctest/doctest.h"

#include "MemoryHealth.h"

TEST_CASE("boot mem rules differ between boot0 and bootN")
{
	MemSample okSample{15000, 15000, 0};
	CHECK(evaluateBootMem(BootMemStage::Boot0, okSample) == MemLevel::Ok);
	CHECK(evaluateBootMem(BootMemStage::Boot1, okSample) == MemLevel::Ok);

	MemSample boot0Warn{13999, 15000, 0};
	CHECK(evaluateBootMem(BootMemStage::Boot0, boot0Warn) == MemLevel::Warn);
	CHECK(evaluateBootMem(BootMemStage::Boot1, boot0Warn) == MemLevel::Ok);

	MemSample bootNCrit{5000, 2000, 40};
	CHECK(evaluateBootMem(BootMemStage::Boot2, bootNCrit) == MemLevel::Crit);
}

TEST_CASE("runtime mem rules use runtime thresholds")
{
	MemSample okSample{7000, 5000, 10};
	CHECK(evaluateRuntimeMem(okSample) == MemLevel::Ok);

	MemSample warnSample{6000, 5000, 26};
	CHECK(evaluateRuntimeMem(warnSample) == MemLevel::Warn);

	MemSample critSample{3000, 3000, 40};
	CHECK(evaluateRuntimeMem(critSample) == MemLevel::Crit);
}

TEST_CASE("boot worst latch keeps highest severity")
{
	BootMemWorst worst{MemLevel::Ok, BootMemStage::Boot0, {0, 0, 0}};
	MemSample warnSample{13000, 13000, 0};
	MemSample critSample{3000, 2000, 40};

	CHECK(updateBootMemWorst(worst, BootMemStage::Boot0, warnSample, MemLevel::Warn));
	CHECK(worst.level == MemLevel::Warn);
	CHECK(updateBootMemWorst(worst, BootMemStage::Boot2, critSample, MemLevel::Crit));
	CHECK(worst.level == MemLevel::Crit);
	CHECK_FALSE(updateBootMemWorst(worst, BootMemStage::Boot1, warnSample, MemLevel::Warn));
}
