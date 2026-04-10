/*
  Rs485RuntimeReconnect.h

  Pure helper logic for deciding when runtime RS485 loss should restart
  baud/identity discovery.
*/
#pragma once

#include <cstdint>

constexpr uint8_t kRs485RuntimeLossThreshold = 3;

struct Rs485RuntimeReconnectTracker {
	uint32_t connectionEpoch = 0;
	uint8_t consecutiveTransportFailures = 0;
	bool liveSnapshotSeenInEpoch = false;
};

struct Rs485RuntimeReconnectEvaluation {
	bool triggerRediscovery = false;
	uint8_t consecutiveTransportFailures = 0;
	bool liveSnapshotSeenInEpoch = false;
};

static inline void
rs485RuntimeReconnectOnConnected(Rs485RuntimeReconnectTracker &tracker)
{
	tracker.connectionEpoch++;
	tracker.consecutiveTransportFailures = 0;
	tracker.liveSnapshotSeenInEpoch = false;
}

static inline void
rs485RuntimeReconnectOnRediscoveryStart(Rs485RuntimeReconnectTracker &tracker)
{
	tracker.consecutiveTransportFailures = 0;
	tracker.liveSnapshotSeenInEpoch = false;
}

static inline Rs485RuntimeReconnectEvaluation
rs485EvaluateRuntimeReconnect(bool connectedState,
                              const Rs485RuntimeReconnectTracker &tracker,
                              bool snapshotSucceeded,
                              bool snapshotTransportFailure,
                              bool rs485OnlineAfterSnapshot,
                              uint8_t lossThreshold = kRs485RuntimeLossThreshold)
{
	Rs485RuntimeReconnectEvaluation evaluation{};

	if (!connectedState) {
		return evaluation;
	}

	if (snapshotSucceeded) {
		evaluation.liveSnapshotSeenInEpoch = true;
		return evaluation;
	}

	if (!tracker.liveSnapshotSeenInEpoch) {
		return evaluation;
	}

	const bool transportLoss = snapshotTransportFailure && !rs485OnlineAfterSnapshot;
	if (!transportLoss) {
		evaluation.liveSnapshotSeenInEpoch = true;
		return evaluation;
	}

	evaluation.liveSnapshotSeenInEpoch = true;
	if (tracker.consecutiveTransportFailures < UINT8_MAX) {
		evaluation.consecutiveTransportFailures =
			static_cast<uint8_t>(tracker.consecutiveTransportFailures + 1);
	} else {
		evaluation.consecutiveTransportFailures = UINT8_MAX;
	}

	if (lossThreshold > 0 && evaluation.consecutiveTransportFailures >= lossThreshold) {
		evaluation.triggerRediscovery = true;
		evaluation.consecutiveTransportFailures = 0;
		evaluation.liveSnapshotSeenInEpoch = false;
	}

	return evaluation;
}
