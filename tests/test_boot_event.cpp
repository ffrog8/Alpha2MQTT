// Purpose: Verify retained boot-event publish state only advances after a
// successful MQTT publish, so transient failures remain retryable.

#include "doctest/doctest.h"

#include "BootEvent.h"

TEST_CASE("boot event publish only attempts while connected and unsent")
{
	CHECK(shouldPublishBootEvent(false, true));
	CHECK_FALSE(shouldPublishBootEvent(true, true));
	CHECK_FALSE(shouldPublishBootEvent(false, false));
}

TEST_CASE("boot event remains retryable after a failed publish")
{
	CHECK_FALSE(bootEventPublishedAfterAttempt(false, false));
	CHECK(bootEventPublishedAfterAttempt(false, true));
	CHECK(bootEventPublishedAfterAttempt(true, false));
}
