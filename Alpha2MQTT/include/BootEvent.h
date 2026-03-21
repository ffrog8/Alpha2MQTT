// Purpose: Keep retained boot-event publish state transitions testable on host.
// Responsibilities: Decide when the firmware should attempt the once-per-boot
// retained publish and when a publish attempt actually consumes that event.
// Invariants: Failed publishes must not mark the boot event as delivered.
#pragma once

bool shouldPublishBootEvent(bool alreadyPublished, bool mqttConnected);
bool bootEventPublishedAfterAttempt(bool alreadyPublished, bool publishSucceeded);
