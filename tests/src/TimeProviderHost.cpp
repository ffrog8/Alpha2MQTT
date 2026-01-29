#include "TimeProvider.h"
#include "TimeProviderFake.h"

static uint32_t g_fakeMillis = 0;

uint32_t nowMillis()
{
	return g_fakeMillis;
}

void setFakeMillis(uint32_t now)
{
	g_fakeMillis = now;
}
