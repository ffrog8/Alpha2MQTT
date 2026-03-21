/*
  Rs485StubLogic.h

  Pure helper logic for the RS485/Modbus stub backend (no Arduino deps).
*/
#pragma once

#include <cstdlib>
#include <cstdint>
#include <cstring>

enum class Rs485StubMode : uint8_t {
	OfflineForever = 0,
	OnlineAlways = 1,
	FailFirstNThenRecover = 2,
	// Fail while in the "offline" window, then succeed while in the "online" window, repeating.
	FlapTime = 3,
	// Fail probe/reads until a minimum number of probe attempts has occurred, then succeed.
	ProbeDelayedOnline = 4,
};

enum class Rs485StubFailType : uint8_t {
	NoResponse = 0,
	SlaveError = 1,
};

struct Rs485StubConfig {
	Rs485StubMode mode = Rs485StubMode::OfflineForever;
	uint32_t failFirstN = 0;
	// When non-zero, fail reads that touch this starting register.
	uint16_t failRegister = 0;
	Rs485StubFailType failType = Rs485StubFailType::NoResponse;
	// Optional simulated latency (ms) per Modbus request. Uses the service hook to keep the main loop alive.
	uint16_t latencyMs = 0;

	// When true, unknown/unimplemented register reads fail (instead of returning deterministic pseudo-data).
	bool strictUnknownRegisters = false;

	// Optional deterministic failure pattern: every Nth snapshot attempt fails (1-based).
	// Applies only while _inSnapshot is true.
	uint32_t failEveryN = 0;

	// Optional: fail only reads and/or only writes.
	bool failReads = true;
	bool failWrites = true;

	// Optional: fail for the first N milliseconds after applying control.
	uint32_t failForMs = 0;

	// Flap windows (ms) for Rs485StubMode::FlapTime.
	uint32_t flapOnlineMs = 0;
	uint32_t flapOfflineMs = 0;

	// Probe-delayed online for Rs485StubMode::ProbeDelayedOnline.
	uint32_t probeSuccessAfterN = 0;
};

static inline bool
rs485StubShouldFail(const Rs485StubConfig &cfg, uint32_t attemptIndexOneBased, uint16_t startRegister)
{
	if (cfg.failRegister != 0 && startRegister == cfg.failRegister) {
		return true;
	}

	switch (cfg.mode) {
	case Rs485StubMode::OfflineForever:
		return true;
	case Rs485StubMode::OnlineAlways:
		return false;
	case Rs485StubMode::FailFirstNThenRecover:
		return attemptIndexOneBased <= cfg.failFirstN;
	case Rs485StubMode::FlapTime:
	case Rs485StubMode::ProbeDelayedOnline:
		// Determined by the handler's runtime state (time/probe count), not purely by attempt index.
		return false;
	default:
		return true;
	}
}

static inline bool
rs485StubModeUsesProbeLifecycle(Rs485StubMode mode)
{
	switch (mode) {
	case Rs485StubMode::OfflineForever:
	case Rs485StubMode::ProbeDelayedOnline:
		return true;
	case Rs485StubMode::OnlineAlways:
	case Rs485StubMode::FailFirstNThenRecover:
	case Rs485StubMode::FlapTime:
		return false;
	default:
		return true;
	}
}

static inline bool
rs485StubParseModeField(const char *payload, Rs485StubMode &mode)
{
	if (payload == nullptr) {
		return false;
	}

	const char *pos = strstr(payload, "\"mode\"");
	if (pos == nullptr) {
		return false;
	}

	pos = strchr(pos, ':');
	if (pos == nullptr) {
		return false;
	}

	pos++;
	while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') {
		pos++;
	}
	if (*pos != '"') {
		return false;
	}
	pos++;

	char modeBuf[24] = { 0 };
	size_t idx = 0;
	while (*pos != '\0' && *pos != '"' && idx + 1 < sizeof(modeBuf)) {
		modeBuf[idx++] = *pos++;
	}
	if (*pos != '"') {
		return false;
	}
	modeBuf[idx] = '\0';

	if (!strcmp(modeBuf, "online")) {
		mode = Rs485StubMode::OnlineAlways;
		return true;
	}
	if (!strcmp(modeBuf, "offline")) {
		mode = Rs485StubMode::OfflineForever;
		return true;
	}
	if (!strcmp(modeBuf, "fail") || !strcmp(modeBuf, "fail_then_recover")) {
		mode = Rs485StubMode::FailFirstNThenRecover;
		return true;
	}
	if (!strcmp(modeBuf, "flap")) {
		mode = Rs485StubMode::FlapTime;
		return true;
	}
	if (!strcmp(modeBuf, "probe_delayed")) {
		mode = Rs485StubMode::ProbeDelayedOnline;
		return true;
	}
	return false;
}

static inline bool
rs485StubParseIntField(const char *payload, const char *key, int32_t &out)
{
	if (payload == nullptr || key == nullptr) {
		return false;
	}

	const size_t keyLen = strlen(key);
	const auto isTokenChar = [](char ch) -> bool {
		return (ch >= '0' && ch <= '9') ||
		       (ch >= 'A' && ch <= 'Z') ||
		       (ch >= 'a' && ch <= 'z') ||
		       ch == '_';
	};

	const char *search = payload;
	while (const char *pos = strstr(search, key)) {
		const char prev = (pos == payload) ? '\0' : pos[-1];
		const char next = pos[keyLen];
		if ((pos == payload || !isTokenChar(prev)) &&
		    (next == '\0' || !isTokenChar(next))) {
			pos += keyLen;
			while (*pos == ' ' || *pos == ':' || *pos == '=' || *pos == '"') {
				pos++;
			}
			if (*pos == '\0') {
				return false;
			}

			char *endPtr = nullptr;
			long parsed = strtol(pos, &endPtr, 10);
			if (endPtr == pos) {
				return false;
			}
			out = static_cast<int32_t>(parsed);
			return true;
		}
		search = pos + keyLen;
	}
	return false;
}

static inline uint16_t
rs485StubWordForRegister(uint16_t reg)
{
	// Deterministic pseudo data: stable across boots and builds.
	return static_cast<uint16_t>(reg ^ 0xA55A);
}
