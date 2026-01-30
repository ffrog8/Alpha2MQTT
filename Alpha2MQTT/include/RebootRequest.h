// Purpose: Provide a minimal reboot request helper for persisting boot mode/intent.
// Responsibilities: Enforce write order (intent before mode) and invoke restart hook.
// Invariants: No Arduino dependencies; caller supplies persistence/restart hooks.
#pragma once

#include "BootModes.h"

class RebootRequestStore {
public:
	virtual ~RebootRequestStore() = default;
	virtual void writeBootIntent(BootIntent intent) = 0;
	virtual void writeBootMode(BootMode mode) = 0;
};

using RestartHook = void (*)();

void requestReboot(RebootRequestStore &store, BootMode mode, BootIntent intent, RestartHook restartHook);
