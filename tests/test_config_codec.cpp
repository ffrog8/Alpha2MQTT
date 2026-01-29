#include "doctest/doctest.h"

#include "ConfigCodec.h"

TEST_CASE("config codec round-trips")
{
	Config input;
	input.pollIntervalSeconds = 120;
	input.bootMode = BootMode::WifiConfig;
	input.enabledRegisterMask = 0xA5A5u;

	std::string encoded = serializeConfig(input);
	Config decoded = deserializeConfig(encoded);

	CHECK(decoded.pollIntervalSeconds == 120);
	CHECK(decoded.bootMode == BootMode::WifiConfig);
	CHECK(decoded.enabledRegisterMask == 0xA5A5u);
}

TEST_CASE("config codec applies defaults when keys are missing")
{
	Config decoded = deserializeConfig("boot_mode=ap_config");
	CHECK(decoded.bootMode == BootMode::ApConfig);
	CHECK(decoded.pollIntervalSeconds == kPollIntervalDefaultSeconds);
	CHECK(decoded.enabledRegisterMask == 0u);
}

TEST_CASE("config codec clamps poll interval")
{
	Config decodedMin = deserializeConfig("poll_interval_s=0");
	CHECK(decodedMin.pollIntervalSeconds == kPollIntervalMinSeconds);

	Config decodedMax = deserializeConfig("poll_interval_s=999999");
	CHECK(decodedMax.pollIntervalSeconds == kPollIntervalMaxSeconds);
}
