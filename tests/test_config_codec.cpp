#include "doctest/doctest.h"

#include "ConfigCodec.h"

TEST_CASE("config codec round-trips")
{
	A2mConfig input;
	input.pollIntervalSeconds = 120;
	input.bootMode = BootMode::WifiConfig;
	input.bootIntent = BootIntent::WifiConfig;
	input.enabledRegisterMask = 0xA5A5u;

	std::string encoded = serializeConfig(input);
	A2mConfig decoded = deserializeConfig(encoded);

	CHECK(decoded.pollIntervalSeconds == 120);
	CHECK(decoded.bootMode == BootMode::WifiConfig);
	CHECK(decoded.bootIntent == BootIntent::WifiConfig);
	CHECK(decoded.enabledRegisterMask == 0xA5A5u);
}

TEST_CASE("config codec applies defaults when keys are missing")
{
	A2mConfig decoded = deserializeConfig("boot_mode=ap_config");
	CHECK(decoded.bootMode == BootMode::ApConfig);
	CHECK(decoded.bootIntent == BootIntent::Normal);
	CHECK(decoded.pollIntervalSeconds == kPollIntervalDefaultSeconds);
	CHECK(decoded.enabledRegisterMask == 0u);
}

TEST_CASE("config codec clamps poll interval")
{
	A2mConfig decodedMin = deserializeConfig("poll_interval_s=0");
	CHECK(decodedMin.pollIntervalSeconds == kPollIntervalMinSeconds);

	A2mConfig decodedMax = deserializeConfig("poll_interval_s=999999");
	CHECK(decodedMax.pollIntervalSeconds == kPollIntervalMaxSeconds);
}

TEST_CASE("consume boot intent resets to normal")
{
	A2mConfig config = defaultConfig();
	config.bootIntent = BootIntent::WifiConfig;

	BootIntent prior = consumeBootIntent(config);

	CHECK(prior == BootIntent::WifiConfig);
	CHECK(config.bootIntent == BootIntent::Normal);
}
