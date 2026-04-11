/*
  Rs485BaudSync.h

  Pure helper logic for persisted RS485 baud configuration, supported value
  mapping, and one-shot per-connection-epoch reconcile policy.
*/
#pragma once

#include <cstdint>

#include "Definitions.h"

enum class Rs485BaudSyncState : uint8_t {
	Unknown = 0,
	Synced,
	Mismatch,
	Failed
};

struct Rs485BaudTracker {
	uint32_t configuredBaud = 0;
	uint32_t actualBaud = 0;
	uint32_t lastWriteAttemptEpoch = 0;
	bool hasConfiguredBaud = false;
	bool pendingConfirmation = false;
	Rs485BaudSyncState syncState = Rs485BaudSyncState::Unknown;
};

static inline const char *
rs485BaudSyncStateLabel(Rs485BaudSyncState state)
{
	switch (state) {
	case Rs485BaudSyncState::Synced:
		return "synced";
	case Rs485BaudSyncState::Mismatch:
		return "mismatch";
	case Rs485BaudSyncState::Failed:
		return "failed";
	case Rs485BaudSyncState::Unknown:
	default:
		return "unknown";
	}
}

static inline bool
rs485BaudValueSupported(uint32_t baud)
{
	return baud == 9600UL || baud == 115200UL || baud == 19200UL;
}

static inline bool
rs485BaudStoredValueUsable(bool hasKey, uint32_t storedBaud)
{
	return hasKey && rs485BaudValueSupported(storedBaud);
}

static inline bool
rs485BaudValueToRegister(uint32_t baud, uint16_t &regValue)
{
	switch (baud) {
	case 9600UL:
		regValue = MODBUS_BAUD_RATE_9600;
		return true;
	case 115200UL:
		regValue = MODBUS_BAUD_RATE_115200;
		return true;
	case 19200UL:
		regValue = MODBUS_BAUD_RATE_19200;
		return true;
	default:
		return false;
	}
}

static inline bool
rs485BaudRegisterToValue(uint16_t regValue, uint32_t &baud)
{
	switch (regValue) {
	case MODBUS_BAUD_RATE_9600:
		baud = 9600UL;
		return true;
	case MODBUS_BAUD_RATE_115200:
		baud = 115200UL;
		return true;
	case MODBUS_BAUD_RATE_19200:
		baud = 19200UL;
		return true;
	case MODBUS_BAUD_RATE_256000:
		baud = 256000UL;
		return true;
	default:
		return false;
	}
}

static inline void
rs485BaudTrackerOnConnected(Rs485BaudTracker &tracker)
{
	tracker.actualBaud = 0;
	if (!tracker.pendingConfirmation) {
		tracker.syncState = Rs485BaudSyncState::Unknown;
	}
}

static inline bool
rs485BaudTrackerNeedsWriteAttempt(const Rs485BaudTracker &tracker, uint32_t connectionEpoch)
{
	return tracker.hasConfiguredBaud && tracker.actualBaud != 0 &&
	       tracker.configuredBaud != tracker.actualBaud && !tracker.pendingConfirmation &&
	       tracker.lastWriteAttemptEpoch != connectionEpoch;
}

static inline bool
rs485BaudTrackerNeedsObservation(const Rs485BaudTracker &tracker, uint32_t connectionEpoch)
{
	return tracker.pendingConfirmation || tracker.actualBaud == 0 ||
	       rs485BaudTrackerNeedsWriteAttempt(tracker, connectionEpoch);
}

static inline void
rs485BaudTrackerMarkObserved(Rs485BaudTracker &tracker, uint32_t liveBaud)
{
	tracker.actualBaud = liveBaud;
	if (!tracker.hasConfiguredBaud) {
		tracker.syncState = Rs485BaudSyncState::Synced;
		return;
	}
	tracker.syncState =
		(tracker.configuredBaud == liveBaud) ? Rs485BaudSyncState::Synced : Rs485BaudSyncState::Mismatch;
}

static inline void
rs485BaudTrackerMarkWriteAttempt(Rs485BaudTracker &tracker, uint32_t connectionEpoch)
{
	tracker.lastWriteAttemptEpoch = connectionEpoch;
	tracker.pendingConfirmation = true;
	tracker.syncState = Rs485BaudSyncState::Mismatch;
}

static inline void
rs485BaudTrackerMarkConfirmationResult(Rs485BaudTracker &tracker, uint32_t connectionEpoch, bool synced)
{
	tracker.lastWriteAttemptEpoch = connectionEpoch;
	tracker.pendingConfirmation = false;
	tracker.syncState = synced ? Rs485BaudSyncState::Synced : Rs485BaudSyncState::Failed;
}

static inline bool
rs485ShouldRunAutoBaudRecoverySweep(const Rs485BaudTracker &tracker,
                                    uint32_t connectionEpoch,
                                    uint8_t identityReadFailures,
                                    bool recoveryAttempted)
{
	return !tracker.hasConfiguredBaud && connectionEpoch == 0 && identityReadFailures >= 4 &&
	       !recoveryAttempted;
}
