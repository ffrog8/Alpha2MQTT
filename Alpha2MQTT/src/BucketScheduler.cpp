// Purpose: Build per-frequency publish buckets at runtime without scanning all entities each loop.
#include "../include/BucketScheduler.h"

BucketMembership
buildBucketMembership(const MqttEntityRuntime *rt,
                      size_t entityCount,
                      uint16_t *membersTenSec,
                      uint16_t *membersOneMin,
                      uint16_t *membersFiveMin,
                      uint16_t *membersOneHour,
                      uint16_t *membersOneDay,
                      NeedsEssSnapshotFn needsEssSnapshot)
{
	BucketMembership out{};
	if (rt == nullptr || entityCount == 0) {
		return out;
	}

	for (size_t i = 0; i < entityCount; ++i) {
		const bool needsSnapshot = (needsEssSnapshot != nullptr) ? needsEssSnapshot(i) : false;
		switch (rt[i].effectiveFreq) {
		case mqttUpdateFreq::freqTenSec:
			if (membersTenSec != nullptr) {
				membersTenSec[out.tenSecCount] = static_cast<uint16_t>(i);
			}
			out.tenSecCount++;
			out.tenSecHasEssSnapshot = out.tenSecHasEssSnapshot || needsSnapshot;
			break;
		case mqttUpdateFreq::freqOneMin:
			if (membersOneMin != nullptr) {
				membersOneMin[out.oneMinCount] = static_cast<uint16_t>(i);
			}
			out.oneMinCount++;
			out.oneMinHasEssSnapshot = out.oneMinHasEssSnapshot || needsSnapshot;
			break;
		case mqttUpdateFreq::freqFiveMin:
			if (membersFiveMin != nullptr) {
				membersFiveMin[out.fiveMinCount] = static_cast<uint16_t>(i);
			}
			out.fiveMinCount++;
			out.fiveMinHasEssSnapshot = out.fiveMinHasEssSnapshot || needsSnapshot;
			break;
		case mqttUpdateFreq::freqOneHour:
			if (membersOneHour != nullptr) {
				membersOneHour[out.oneHourCount] = static_cast<uint16_t>(i);
			}
			out.oneHourCount++;
			out.oneHourHasEssSnapshot = out.oneHourHasEssSnapshot || needsSnapshot;
			break;
		case mqttUpdateFreq::freqOneDay:
			if (membersOneDay != nullptr) {
				membersOneDay[out.oneDayCount] = static_cast<uint16_t>(i);
			}
			out.oneDayCount++;
			out.oneDayHasEssSnapshot = out.oneDayHasEssSnapshot || needsSnapshot;
			break;
		case mqttUpdateFreq::freqNever:
		case mqttUpdateFreq::freqDisabled:
		default:
			break;
		}
	}

	return out;
}

