// Purpose: Verify reboot request persistence order and restart behavior.
#include <string>
#include <vector>

#include "doctest/doctest.h"

#include "RebootRequest.h"

namespace {
struct FakeStore : RebootRequestStore {
	BootIntent intent = BootIntent::Normal;
	BootMode mode = BootMode::Normal;
	std::vector<std::string> calls;
	int restartCount = 0;

	void writeBootIntent(BootIntent value) override
	{
		intent = value;
		calls.emplace_back("intent");
	}

	void writeBootMode(BootMode value) override
	{
		mode = value;
		calls.emplace_back("mode");
	}
};

FakeStore *activeStore = nullptr;

void recordRestart()
{
	if (activeStore != nullptr) {
		activeStore->restartCount++;
	}
}
} // namespace

TEST_CASE("requestReboot writes intent then mode and restarts (ap config)")
{
	FakeStore store;
	activeStore = &store;

	requestReboot(store, BootMode::ApConfig, BootIntent::ApConfig, recordRestart);

	CHECK(store.intent == BootIntent::ApConfig);
	CHECK(store.mode == BootMode::ApConfig);
	CHECK(store.restartCount == 1);
	REQUIRE(store.calls.size() == 2);
	CHECK(store.calls[0] == "intent");
	CHECK(store.calls[1] == "mode");
}

TEST_CASE("requestReboot writes intent then mode and restarts (wifi config)")
{
	FakeStore store;
	activeStore = &store;

	requestReboot(store, BootMode::WifiConfig, BootIntent::WifiConfig, recordRestart);

	CHECK(store.intent == BootIntent::WifiConfig);
	CHECK(store.mode == BootMode::WifiConfig);
	CHECK(store.restartCount == 1);
	REQUIRE(store.calls.size() == 2);
	CHECK(store.calls[0] == "intent");
	CHECK(store.calls[1] == "mode");
}

TEST_CASE("requestReboot writes intent then mode and restarts (normal)")
{
	FakeStore store;
	activeStore = &store;

	requestReboot(store, BootMode::Normal, BootIntent::Normal, recordRestart);

	CHECK(store.intent == BootIntent::Normal);
	CHECK(store.mode == BootMode::Normal);
	CHECK(store.restartCount == 1);
	REQUIRE(store.calls.size() == 2);
	CHECK(store.calls[0] == "intent");
	CHECK(store.calls[1] == "mode");
}
