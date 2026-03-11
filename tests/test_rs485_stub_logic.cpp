// Purpose: Validate deterministic RS485 stub backend failure policy logic in host tests.
#include "doctest/doctest.h"

#include "Rs485StubLogic.h"

TEST_CASE("rs485 stub: offline forever always fails")
{
	Rs485StubConfig cfg;
	cfg.mode = Rs485StubMode::OfflineForever;
	CHECK(rs485StubShouldFail(cfg, 1, 1234));
	CHECK(rs485StubShouldFail(cfg, 100, 1234));
}

TEST_CASE("rs485 stub: online always never fails")
{
	Rs485StubConfig cfg;
	cfg.mode = Rs485StubMode::OnlineAlways;
	CHECK_FALSE(rs485StubShouldFail(cfg, 1, 1234));
	CHECK_FALSE(rs485StubShouldFail(cfg, 100, 1234));
}

TEST_CASE("rs485 stub: fail first N then recover")
{
	Rs485StubConfig cfg;
	cfg.mode = Rs485StubMode::FailFirstNThenRecover;
	cfg.failFirstN = 3;

	CHECK(rs485StubShouldFail(cfg, 1, 1234));
	CHECK(rs485StubShouldFail(cfg, 2, 1234));
	CHECK(rs485StubShouldFail(cfg, 3, 1234));
	CHECK_FALSE(rs485StubShouldFail(cfg, 4, 1234));
	CHECK_FALSE(rs485StubShouldFail(cfg, 5, 1234));
}

TEST_CASE("rs485 stub: can fail a specific register regardless of mode")
{
	Rs485StubConfig cfg;
	cfg.mode = Rs485StubMode::OnlineAlways;
	cfg.failRegister = 555;

	CHECK(rs485StubShouldFail(cfg, 100, 555));
	CHECK_FALSE(rs485StubShouldFail(cfg, 100, 556));
}

TEST_CASE("rs485 stub: online-like modes bypass probe lifecycle")
{
	CHECK_FALSE(rs485StubModeUsesProbeLifecycle(Rs485StubMode::OnlineAlways));
	CHECK_FALSE(rs485StubModeUsesProbeLifecycle(Rs485StubMode::FailFirstNThenRecover));
	CHECK_FALSE(rs485StubModeUsesProbeLifecycle(Rs485StubMode::FlapTime));
}

TEST_CASE("rs485 stub: offline-like modes keep probe lifecycle")
{
	CHECK(rs485StubModeUsesProbeLifecycle(Rs485StubMode::OfflineForever));
	CHECK(rs485StubModeUsesProbeLifecycle(Rs485StubMode::ProbeDelayedOnline));
}
