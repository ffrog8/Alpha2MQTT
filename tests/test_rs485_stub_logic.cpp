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

TEST_CASE("rs485 stub: parse mode from explicit mode field only")
{
	Rs485StubMode mode = Rs485StubMode::OfflineForever;
	CHECK(rs485StubParseModeField("{\"mode\":\"online\"}", mode));
	CHECK(mode == Rs485StubMode::OnlineAlways);
	CHECK(rs485StubParseModeField("{\"mode\":\"fail_then_recover\"}", mode));
	CHECK(mode == Rs485StubMode::FailFirstNThenRecover);
	CHECK(rs485StubParseModeField("{\"mode\": \"probe_delayed\"}", mode));
	CHECK(mode == Rs485StubMode::ProbeDelayedOnline);
}

TEST_CASE("rs485 stub: advanced keys do not imply mode")
{
	Rs485StubMode mode = Rs485StubMode::OfflineForever;
	CHECK_FALSE(rs485StubParseModeField("{\"flap_online_ms\":3000,\"flap_offline_ms\":3000}", mode));
	CHECK(mode == Rs485StubMode::OfflineForever);
	CHECK_FALSE(rs485StubParseModeField("{\"fail_every_n\":2}", mode));
	CHECK(mode == Rs485StubMode::OfflineForever);
	CHECK_FALSE(rs485StubParseModeField("{\"mode_hint\":\"online\"}", mode));
	CHECK(mode == Rs485StubMode::OfflineForever);
}

TEST_CASE("rs485 stub: invalid mode rejects payload even when other keys are present")
{
	Rs485StubMode mode = Rs485StubMode::OnlineAlways;
	CHECK_FALSE(rs485StubParseModeField("{\"mode\":\"typo\",\"fail_n\":2,\"latency_ms\":50}", mode));
	CHECK(mode == Rs485StubMode::OnlineAlways);
}

TEST_CASE("rs485 stub: integer field parsing does not steal unrelated numeric values")
{
	int32_t value = 0;
	CHECK_FALSE(rs485StubParseIntField("{\"mode\":\"fail\",\"reg\":123}", "fail_n", value));
	CHECK(rs485StubParseIntField("{\"fail_n\":2,\"reg\":123}", "fail_n", value));
	CHECK(value == 2);
	CHECK_FALSE(rs485StubParseIntField("{\"dispatch_soc\":55}", "soc", value));
}
