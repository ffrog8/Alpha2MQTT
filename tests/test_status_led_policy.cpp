// Purpose: Lock down status LED render behavior so MQTT activity pulses remain
// visible without regressing the base health-state mapping.

#include <doctest/doctest.h>

#include "StatusLedPolicy.h"

TEST_CASE("status led: base healthy state is green and off on monochrome builds")
{
	const StatusLedRender render = computeStatusLedRender(true, true, true, false);
	CHECK(render.red == 0);
	CHECK(render.green == 255);
	CHECK(render.blue == 0);
	CHECK_FALSE(render.monochromeOn);
}

TEST_CASE("status led: mqtt pulse overrides to blue on rgb and turns monochrome on")
{
	const StatusLedRender render = computeStatusLedRender(true, true, true, true);
	CHECK(render.red == 0);
	CHECK(render.green == 0);
	CHECK(render.blue == 255);
	CHECK(render.monochromeOn);
}

TEST_CASE("status led: disconnected wifi remains red without mqtt pulse")
{
	const StatusLedRender render = computeStatusLedRender(false, false, false, false);
	CHECK(render.red == 255);
	CHECK(render.green == 0);
	CHECK(render.blue == 0);
	CHECK_FALSE(render.monochromeOn);
}
