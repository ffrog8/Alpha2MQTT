#pragma once

#include <cstdint>
#include <string>

#include "BootModes.h"

struct A2mConfig {
	uint32_t pollIntervalSeconds;
	BootMode bootMode;
	// Persist the reboot intent so the next boot can distinguish intentional reboots from crashes.
	BootIntent bootIntent;
	uint64_t enabledRegisterMask;
};

constexpr uint32_t kPollIntervalMinSeconds = 1;
constexpr uint32_t kPollIntervalMaxSeconds = 86400;
constexpr uint32_t kPollIntervalDefaultSeconds = 60;

A2mConfig defaultConfig();
uint32_t clampPollInterval(uint32_t valueSeconds);

std::string serializeConfig(const A2mConfig &config);
A2mConfig deserializeConfig(const std::string &payload);
BootIntent consumeBootIntent(A2mConfig &config);
