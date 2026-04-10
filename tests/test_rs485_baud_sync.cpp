// Purpose: Verify persisted RS485 baud mapping and one-shot reconcile policy.

#include <doctest/doctest.h>

#include "Rs485BaudSync.h"

TEST_CASE("rs485 baud sync: supported numeric baud values map to register enums")
{
	uint16_t regValue = 0;
	CHECK(rs485BaudValueToRegister(9600, regValue));
	CHECK(regValue == MODBUS_BAUD_RATE_9600);
	CHECK(rs485BaudValueToRegister(115200, regValue));
	CHECK(regValue == MODBUS_BAUD_RATE_115200);
	CHECK(rs485BaudValueToRegister(19200, regValue));
	CHECK(regValue == MODBUS_BAUD_RATE_19200);
	CHECK_FALSE(rs485BaudValueToRegister(256000, regValue));
}

TEST_CASE("rs485 baud sync: supported register enums map back to numeric baud values")
{
	uint32_t baud = 0;
	CHECK(rs485BaudRegisterToValue(MODBUS_BAUD_RATE_9600, baud));
	CHECK(baud == 9600);
	CHECK(rs485BaudRegisterToValue(MODBUS_BAUD_RATE_115200, baud));
	CHECK(baud == 115200);
	CHECK(rs485BaudRegisterToValue(MODBUS_BAUD_RATE_19200, baud));
	CHECK(baud == 19200);
	CHECK(rs485BaudRegisterToValue(MODBUS_BAUD_RATE_256000, baud));
	CHECK(baud == 256000);
	CHECK_FALSE(rs485BaudValueSupported(256000));
}

TEST_CASE("rs485 baud sync: stored value is usable only when key exists and baud is supported")
{
	CHECK(rs485BaudStoredValueUsable(true, 9600));
	CHECK(rs485BaudStoredValueUsable(true, 115200));
	CHECK_FALSE(rs485BaudStoredValueUsable(false, 9600));
	CHECK_FALSE(rs485BaudStoredValueUsable(true, 256000));
}

TEST_CASE("rs485 baud sync: first live baud can seed configured state")
{
	Rs485BaudTracker tracker{};
	rs485BaudTrackerMarkSeeded(tracker, 115200);
	CHECK(tracker.hasConfiguredBaud);
	CHECK(tracker.configuredBaud == 115200);
	CHECK(tracker.actualBaud == 115200);
	CHECK(tracker.syncState == Rs485BaudSyncState::Synced);
}

TEST_CASE("rs485 baud sync: unconfigured tracker observes only once per connection epoch")
{
	Rs485BaudTracker tracker{};
	CHECK(rs485BaudTrackerNeedsObservation(tracker, 2));

	rs485BaudTrackerMarkObserved(tracker, 9600);
	CHECK_FALSE(rs485BaudTrackerNeedsObservation(tracker, 2));

	rs485BaudTrackerOnConnected(tracker);
	CHECK(rs485BaudTrackerNeedsObservation(tracker, 3));
}

TEST_CASE("rs485 baud sync: mismatch triggers one write attempt per connection epoch")
{
	Rs485BaudTracker tracker{};
	tracker.hasConfiguredBaud = true;
	tracker.configuredBaud = 115200;
	rs485BaudTrackerOnConnected(tracker);
	rs485BaudTrackerMarkObserved(tracker, 9600);

	CHECK(rs485BaudTrackerNeedsWriteAttempt(tracker, 4));
	rs485BaudTrackerMarkWriteAttempt(tracker, 4);
	CHECK(tracker.pendingConfirmation);
	CHECK_FALSE(rs485BaudTrackerNeedsWriteAttempt(tracker, 4));

	rs485BaudTrackerMarkConfirmationResult(tracker, 5, false);
	CHECK(tracker.syncState == Rs485BaudSyncState::Failed);
	CHECK_FALSE(rs485BaudTrackerNeedsWriteAttempt(tracker, 5));
	CHECK(rs485BaudTrackerNeedsWriteAttempt(tracker, 6));
}

TEST_CASE("rs485 baud sync: matching live baud clears pending confirmation as synced")
{
	Rs485BaudTracker tracker{};
	tracker.hasConfiguredBaud = true;
	tracker.configuredBaud = 19200;
	rs485BaudTrackerMarkWriteAttempt(tracker, 8);
	rs485BaudTrackerOnConnected(tracker);
	rs485BaudTrackerMarkObserved(tracker, 19200);
	rs485BaudTrackerMarkConfirmationResult(tracker, 9, true);

	CHECK_FALSE(tracker.pendingConfirmation);
	CHECK(tracker.syncState == Rs485BaudSyncState::Synced);
	CHECK(tracker.lastWriteAttemptEpoch == 9);
}
