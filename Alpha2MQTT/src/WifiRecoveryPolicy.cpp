#include "../include/WifiRecoveryPolicy.h"

WifiRecoveryTiming
wifiRecoveryTiming(void)
{
#ifdef A2M_WIFI_RECOVERY_FAST_PROFILE
	return { 3000U, 20000U, 60000U };
#else
	return { 30000U, 90000U, 300000U };
#endif
}

WifiFailureClass
classifyWifiFailure(const WifiFailureSignals &signals)
{
	if (signals.connected) {
		return WifiFailureClass::Unknown;
	}
	if (signals.authFailed || signals.missingSsid || signals.connectFailed) {
		return WifiFailureClass::InvalidConfig;
	}
	return WifiFailureClass::Unknown;
}

bool
shouldRebootApOnInitialWifiFailure(BootMode currentMode, WifiFailureClass failureClass)
{
	if (failureClass != WifiFailureClass::InvalidConfig) {
		return false;
	}
	return currentMode == BootMode::Normal || currentMode == BootMode::WifiConfig;
}

bool
shouldRebootApOnRuntimeWifiFailure(BootMode currentMode,
                                   bool hasStoredWifiCredentials,
                                   WifiFailureClass failureClass)
{
	if (!hasStoredWifiCredentials || failureClass != WifiFailureClass::InvalidConfig) {
		return false;
	}
	return currentMode == BootMode::Normal;
}

bool
shouldRebootApAfterStaPortalConnectFailure(bool hasStoredWifiCredentials,
                                           WifiFailureClass failureClass)
{
	return hasStoredWifiCredentials && failureClass == WifiFailureClass::InvalidConfig;
}

bool
shouldRebootNormalAfterApIdle(bool hasStoredWifiCredentials, uint32_t idleMs)
{
	const WifiRecoveryTiming timing = wifiRecoveryTiming();
	return hasStoredWifiCredentials && idleMs >= timing.apIdleTimeoutMs;
}
