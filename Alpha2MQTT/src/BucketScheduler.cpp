// Purpose: Build per-frequency publish buckets at runtime without scanning all entities each loop.
#include "../include/BucketScheduler.h"

#include <cstring>

BucketId
bucketIdFromString(const char *value)
{
	if (value == nullptr || *value == '\0') {
		return BucketId::Unknown;
	}
	if (!strcmp(value, "ten_sec") || !strcmp(value, "freqTenSec")) {
		return BucketId::TenSec;
	}
	if (!strcmp(value, "one_min") || !strcmp(value, "freqOneMin")) {
		return BucketId::OneMin;
	}
	if (!strcmp(value, "five_min") || !strcmp(value, "freqFiveMin")) {
		return BucketId::FiveMin;
	}
	if (!strcmp(value, "one_hour") || !strcmp(value, "freqOneHour")) {
		return BucketId::OneHour;
	}
	if (!strcmp(value, "one_day") || !strcmp(value, "freqOneDay")) {
		return BucketId::OneDay;
	}
	if (!strcmp(value, "user") || !strcmp(value, "freqUser")) {
		return BucketId::User;
	}
	if (!strcmp(value, "disabled") || !strcmp(value, "freqDisabled")) {
		return BucketId::Disabled;
	}
	return BucketId::Unknown;
}

const char *
bucketIdToString(BucketId bucket)
{
	switch (bucket) {
	case BucketId::TenSec:
		return "ten_sec";
	case BucketId::OneMin:
		return "one_min";
	case BucketId::FiveMin:
		return "five_min";
	case BucketId::OneHour:
		return "one_hour";
	case BucketId::OneDay:
		return "one_day";
	case BucketId::User:
		return "user";
	case BucketId::Disabled:
		return "disabled";
	default:
		return "unknown";
	}
}

BucketId
bucketIdFromFreq(mqttUpdateFreq freq)
{
	switch (freq) {
	case mqttUpdateFreq::freqTenSec:
		return BucketId::TenSec;
	case mqttUpdateFreq::freqOneMin:
		return BucketId::OneMin;
	case mqttUpdateFreq::freqFiveMin:
		return BucketId::FiveMin;
	case mqttUpdateFreq::freqOneHour:
		return BucketId::OneHour;
	case mqttUpdateFreq::freqOneDay:
		return BucketId::OneDay;
	case mqttUpdateFreq::freqUser:
		return BucketId::User;
	case mqttUpdateFreq::freqDisabled:
		return BucketId::Disabled;
	case mqttUpdateFreq::freqNever:
	default:
		return BucketId::Disabled;
	}
}

mqttUpdateFreq
bucketIdToFreq(BucketId bucket)
{
	switch (bucket) {
	case BucketId::TenSec:
		return mqttUpdateFreq::freqTenSec;
	case BucketId::OneMin:
		return mqttUpdateFreq::freqOneMin;
	case BucketId::FiveMin:
		return mqttUpdateFreq::freqFiveMin;
	case BucketId::OneHour:
		return mqttUpdateFreq::freqOneHour;
	case BucketId::OneDay:
		return mqttUpdateFreq::freqOneDay;
	case BucketId::User:
		return mqttUpdateFreq::freqUser;
	case BucketId::Disabled:
		return mqttUpdateFreq::freqDisabled;
	default:
		return mqttUpdateFreq::freqDisabled;
	}
}

BucketMembership
buildBucketMembership(const BucketId *buckets,
                      size_t entityCount,
                      uint16_t *membersTenSec,
                      uint16_t *membersOneMin,
                      uint16_t *membersFiveMin,
                      uint16_t *membersOneHour,
                      uint16_t *membersOneDay,
                      uint16_t *membersUser,
                      NeedsEssSnapshotFn needsEssSnapshot)
{
	BucketMembership out{};
	if (buckets == nullptr || entityCount == 0) {
		return out;
	}

	for (size_t i = 0; i < entityCount; ++i) {
		const bool needsSnapshot = (needsEssSnapshot != nullptr) ? needsEssSnapshot(i) : false;
		switch (buckets[i]) {
		case BucketId::TenSec:
			if (membersTenSec != nullptr) {
				membersTenSec[out.tenSecCount] = static_cast<uint16_t>(i);
			}
			out.tenSecCount++;
			out.tenSecHasEssSnapshot = out.tenSecHasEssSnapshot || needsSnapshot;
			break;
		case BucketId::OneMin:
			if (membersOneMin != nullptr) {
				membersOneMin[out.oneMinCount] = static_cast<uint16_t>(i);
			}
			out.oneMinCount++;
			out.oneMinHasEssSnapshot = out.oneMinHasEssSnapshot || needsSnapshot;
			break;
		case BucketId::FiveMin:
			if (membersFiveMin != nullptr) {
				membersFiveMin[out.fiveMinCount] = static_cast<uint16_t>(i);
			}
			out.fiveMinCount++;
			out.fiveMinHasEssSnapshot = out.fiveMinHasEssSnapshot || needsSnapshot;
			break;
		case BucketId::OneHour:
			if (membersOneHour != nullptr) {
				membersOneHour[out.oneHourCount] = static_cast<uint16_t>(i);
			}
			out.oneHourCount++;
			out.oneHourHasEssSnapshot = out.oneHourHasEssSnapshot || needsSnapshot;
			break;
		case BucketId::OneDay:
			if (membersOneDay != nullptr) {
				membersOneDay[out.oneDayCount] = static_cast<uint16_t>(i);
			}
			out.oneDayCount++;
			out.oneDayHasEssSnapshot = out.oneDayHasEssSnapshot || needsSnapshot;
			break;
		case BucketId::User:
			if (membersUser != nullptr) {
				membersUser[out.userCount] = static_cast<uint16_t>(i);
			}
			out.userCount++;
			out.userHasEssSnapshot = out.userHasEssSnapshot || needsSnapshot;
			break;
		case BucketId::Disabled:
		case BucketId::Unknown:
		default:
			break;
		}
	}

	return out;
}
