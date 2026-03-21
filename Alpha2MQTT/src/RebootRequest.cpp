// Purpose: Implement the reboot request helper without Arduino dependencies.
// Responsibilities: Persist intent before mode, then invoke restart hook.
// Invariants: Callers provide storage and restart hooks.
#include "../include/RebootRequest.h"

void
requestReboot(RebootRequestStore &store, BootMode mode, BootIntent intent, RestartHook restartHook)
{
	store.writeBootIntent(intent);
	store.writeBootMode(mode);
	if (restartHook != nullptr) {
		restartHook();
	}
}
