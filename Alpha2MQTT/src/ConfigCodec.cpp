#include "../include/ConfigCodec.h"

#include <cerrno>
#include <cinttypes>
#include <cstdlib>

namespace {
const char *kKeyPollInterval = "poll_interval_s";
const char *kKeyBootMode = "boot_mode";
const char *kKeyBootIntent = "boot_intent";
const char *kKeyRegisterMask = "enabled_register_mask";

bool parseUint32(const std::string &value, uint32_t *result)
{
	if (value.empty()) {
		return false;
	}
	char *end = nullptr;
	errno = 0;
	unsigned long parsed = std::strtoul(value.c_str(), &end, 10);
	if (errno != 0 || end == value.c_str() || *end != '\0') {
		return false;
	}
	if (parsed > UINT32_MAX) {
		return false;
	}
	*result = static_cast<uint32_t>(parsed);
	return true;
}

bool parseUint64(const std::string &value, uint64_t *result)
{
	if (value.empty()) {
		return false;
	}
	char *end = nullptr;
	errno = 0;
	unsigned long long parsed = std::strtoull(value.c_str(), &end, 10);
	if (errno != 0 || end == value.c_str() || *end != '\0') {
		return false;
	}
	*result = static_cast<uint64_t>(parsed);
	return true;
}
} // namespace

struct A2mConfig defaultConfig()
{
	return { kPollIntervalDefaultSeconds, BootMode::Normal, BootIntent::Normal, 0 };
}

uint32_t clampPollInterval(uint32_t valueSeconds)
{
	if (valueSeconds < kPollIntervalMinSeconds) {
		return kPollIntervalMinSeconds;
	}
	if (valueSeconds > kPollIntervalMaxSeconds) {
		return kPollIntervalMaxSeconds;
	}
	return valueSeconds;
}

std::string serializeConfig(const A2mConfig &config)
{
	std::string output;
	output.reserve(160);
	output.append(kKeyPollInterval).append("=").append(std::to_string(config.pollIntervalSeconds)).append(";");
	output.append(kKeyBootMode).append("=").append(bootModeToString(config.bootMode)).append(";");
	output.append(kKeyBootIntent).append("=").append(bootIntentToString(config.bootIntent)).append(";");
	output.append(kKeyRegisterMask).append("=").append(std::to_string(config.enabledRegisterMask));
	return output;
}

struct A2mConfig deserializeConfig(const std::string &payload)
{
	A2mConfig config = defaultConfig();

	size_t start = 0;
	while (start < payload.size()) {
		size_t end = payload.find(';', start);
		if (end == std::string::npos) {
			end = payload.size();
		}
		std::string token = payload.substr(start, end - start);
		size_t delimiter = token.find('=');
		if (delimiter != std::string::npos) {
			std::string key = token.substr(0, delimiter);
			std::string value = token.substr(delimiter + 1);
			if (key == kKeyPollInterval) {
				uint32_t parsed = 0;
				if (parseUint32(value, &parsed)) {
					config.pollIntervalSeconds = clampPollInterval(parsed);
				}
			} else if (key == kKeyBootMode) {
				config.bootMode = bootModeFromString(value.c_str());
			} else if (key == kKeyBootIntent) {
				config.bootIntent = bootIntentFromString(value.c_str());
			} else if (key == kKeyRegisterMask) {
				uint64_t parsed = 0;
				if (parseUint64(value, &parsed)) {
					config.enabledRegisterMask = parsed;
				}
			}
		}
		start = end + 1;
	}

	config.pollIntervalSeconds = clampPollInterval(config.pollIntervalSeconds);
	return config;
}

BootIntent consumeBootIntent(A2mConfig &config)
{
	BootIntent prior = config.bootIntent;
	config.bootIntent = BootIntent::Normal;
	return prior;
}
