// Purpose: Validate the runtime RS485 loss policy without pulling in Arduino runtime code.
#include "doctest/doctest.h"

#include "Rs485RuntimeReconnect.h"

TEST_CASE("rs485 runtime reconnect: first successful snapshot arms loss detection for the epoch")
{
	Rs485RuntimeReconnectTracker tracker{};
	rs485RuntimeReconnectOnConnected(tracker);

	const Rs485RuntimeReconnectEvaluation evaluation =
		rs485EvaluateRuntimeReconnect(true, tracker, true, false, true);

	CHECK_FALSE(evaluation.triggerRediscovery);
	CHECK(evaluation.liveSnapshotSeenInEpoch);
	CHECK(evaluation.consecutiveTransportFailures == 0);
}

TEST_CASE("rs485 runtime reconnect: transport failures before a live snapshot do not trigger rediscovery")
{
	Rs485RuntimeReconnectTracker tracker{};
	rs485RuntimeReconnectOnConnected(tracker);

	const Rs485RuntimeReconnectEvaluation evaluation =
		rs485EvaluateRuntimeReconnect(true, tracker, false, true, false);

	CHECK_FALSE(evaluation.triggerRediscovery);
	CHECK_FALSE(evaluation.liveSnapshotSeenInEpoch);
	CHECK(evaluation.consecutiveTransportFailures == 0);
}

TEST_CASE("rs485 runtime reconnect: isolated transport failure after live snapshot increments streak only")
{
	Rs485RuntimeReconnectTracker tracker{};
	rs485RuntimeReconnectOnConnected(tracker);
	tracker.liveSnapshotSeenInEpoch = true;

	const Rs485RuntimeReconnectEvaluation evaluation =
		rs485EvaluateRuntimeReconnect(true, tracker, false, true, false);

	CHECK_FALSE(evaluation.triggerRediscovery);
	CHECK(evaluation.liveSnapshotSeenInEpoch);
	CHECK(evaluation.consecutiveTransportFailures == 1);
}

TEST_CASE("rs485 runtime reconnect: rediscovery triggers after threshold consecutive transport failures")
{
	Rs485RuntimeReconnectTracker tracker{};
	rs485RuntimeReconnectOnConnected(tracker);
	tracker.liveSnapshotSeenInEpoch = true;
	tracker.consecutiveTransportFailures = 2;

	const Rs485RuntimeReconnectEvaluation evaluation =
		rs485EvaluateRuntimeReconnect(true, tracker, false, true, false);

	CHECK(evaluation.triggerRediscovery);
	CHECK_FALSE(evaluation.liveSnapshotSeenInEpoch);
	CHECK(evaluation.consecutiveTransportFailures == 0);
}

TEST_CASE("rs485 runtime reconnect: non-transport failures do not build a rediscovery streak")
{
	Rs485RuntimeReconnectTracker tracker{};
	rs485RuntimeReconnectOnConnected(tracker);
	tracker.liveSnapshotSeenInEpoch = true;
	tracker.consecutiveTransportFailures = 2;

	const Rs485RuntimeReconnectEvaluation evaluation =
		rs485EvaluateRuntimeReconnect(true, tracker, false, false, true);

	CHECK_FALSE(evaluation.triggerRediscovery);
	CHECK(evaluation.liveSnapshotSeenInEpoch);
	CHECK(evaluation.consecutiveTransportFailures == 0);
}

TEST_CASE("rs485 runtime reconnect: success clears failure streak and keeps the epoch armed")
{
	Rs485RuntimeReconnectTracker tracker{};
	rs485RuntimeReconnectOnConnected(tracker);
	tracker.liveSnapshotSeenInEpoch = true;
	tracker.consecutiveTransportFailures = 2;

	const Rs485RuntimeReconnectEvaluation evaluation =
		rs485EvaluateRuntimeReconnect(true, tracker, true, false, true);

	CHECK_FALSE(evaluation.triggerRediscovery);
	CHECK(evaluation.liveSnapshotSeenInEpoch);
	CHECK(evaluation.consecutiveTransportFailures == 0);
}

TEST_CASE("rs485 runtime reconnect: explicit rediscovery reset clears runtime streak state")
{
	Rs485RuntimeReconnectTracker tracker{};
	rs485RuntimeReconnectOnConnected(tracker);
	tracker.liveSnapshotSeenInEpoch = true;
	tracker.consecutiveTransportFailures = 2;

	rs485RuntimeReconnectOnRediscoveryStart(tracker);

	CHECK_FALSE(tracker.liveSnapshotSeenInEpoch);
	CHECK(tracker.consecutiveTransportFailures == 0);
	CHECK(tracker.connectionEpoch == 1);
}
