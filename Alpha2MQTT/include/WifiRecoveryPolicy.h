/*
 * WifiRecoveryPolicy centralizes bounded WiFi recovery timing and decisions.
 * Callers translate platform WiFi state into WifiFailureSignals and keep any
 * latching outside this module so the policy stays pure and host-testable.
 */

#pragma once

#include <cstdint>

#include "BootModes.h"

enum class WifiFailureClass : uint8_t {
	Unknown = 0,
	InvalidConfig = 1,
};

struct WifiFailureSignals {
	bool connected = false;
	bool missingSsid = false;
	bool connectFailed = false;
	bool authFailed = false;
};

struct WifiRecoveryTiming {
	uint32_t bootValidationMs;
	uint32_t runtimeValidationMs;
	uint32_t apIdleTimeoutMs;
};

WifiRecoveryTiming wifiRecoveryTiming(void);
WifiFailureClass classifyWifiFailure(const WifiFailureSignals &signals);
bool shouldRebootApOnInitialWifiFailure(BootMode currentMode, WifiFailureClass failureClass);
bool shouldRebootApOnRuntimeWifiFailure(BootMode currentMode,
                                        bool hasStoredWifiCredentials,
                                        WifiFailureClass failureClass);
bool shouldRebootApAfterStaPortalConnectFailure(bool hasStoredWifiCredentials,
                                                WifiFailureClass failureClass);
bool shouldRebootNormalAfterApIdle(bool hasStoredWifiCredentials, uint32_t idleMs);
