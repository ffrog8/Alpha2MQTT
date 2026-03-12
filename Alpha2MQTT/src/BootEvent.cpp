// Purpose: Keep retained boot-event publish transitions isolated from the
// firmware main loop so host tests can verify retry behavior precisely.
// Responsibilities: Encode the "once per boot" state machine for retained boot
// publishes without depending on Arduino or MQTT client types.

#include "../include/BootEvent.h"

bool
shouldPublishBootEvent(bool alreadyPublished, bool mqttConnected)
{
	return !alreadyPublished && mqttConnected;
}

bool
bootEventPublishedAfterAttempt(bool alreadyPublished, bool publishSucceeded)
{
	return alreadyPublished || publishSucceeded;
}
