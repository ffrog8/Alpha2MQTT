// Purpose: Verify the atomic dispatch MQTT contract stays deterministic and
// mode-safe as firmware control logic evolves.

#include <doctest/doctest.h>

#include <string>

#include "DispatchRequest.h"

TEST_CASE("dispatch request parses and maps state of charge control")
{
	DispatchRequestPayload payload{};
	char error[64] = "";
	REQUIRE(parseDispatchRequestPayload(
		R"({"mode":"state_of_charge_control","power_w":-3000,"soc_percent":20,"duration_s":1800})",
		payload,
		error,
		sizeof(error)));
	CHECK(payload.mode == DispatchRequestMode::StateOfChargeControl);
	CHECK(payload.hasPower);
	CHECK(payload.powerW == -3000);
	CHECK(payload.hasSoc);
	CHECK(payload.socPercent == 20);
	CHECK(payload.hasDuration);
	CHECK(payload.durationS == 1800);

	DispatchRequestPlan plan{};
	REQUIRE(buildDispatchRequestPlan(payload, plan, error, sizeof(error)));
	CHECK_FALSE(plan.stop);
	CHECK(plan.dispatchMode == DISPATCH_MODE_STATE_OF_CHARGE_CONTROL);
	CHECK(plan.dispatchActivePower == DISPATCH_ACTIVE_POWER_OFFSET - 3000);
	CHECK(plan.dispatchSocRaw == 50);
	CHECK(plan.dispatchTimeRaw == 1800);
	CHECK(plan.matchMode);
	CHECK(plan.matchPower);
	CHECK(plan.matchSoc);
	CHECK(plan.matchTime);
}

TEST_CASE("dispatch request rejects invalid or missing required fields")
{
	DispatchRequestPayload payload{};
	char error[64] = "";
	REQUIRE(parseDispatchRequestPayload(
		R"({"mode":"battery_only_charges_from_pv","power_w":500,"duration_s":60})",
		payload,
		error,
		sizeof(error)));
	DispatchRequestPlan plan{};
	CHECK_FALSE(buildDispatchRequestPlan(payload, plan, error, sizeof(error)));
	CHECK(std::string(error) == "invalid power");

	REQUIRE(parseDispatchRequestPayload(
		R"({"mode":"state_of_charge_control","power_w":-1000,"soc_percent":20})",
		payload,
		error,
		sizeof(error)));
	CHECK_FALSE(buildDispatchRequestPlan(payload, plan, error, sizeof(error)));
	CHECK(std::string(error) == "invalid duration");

	CHECK_FALSE(parseDispatchRequestPayload(R"({"mode":"unsupported"})", payload, error, sizeof(error)));
	CHECK(std::string(error) == "invalid mode");
}

TEST_CASE("dispatch request normal mode ignores extra fields and expects stop")
{
	DispatchRequestPayload payload{};
	char error[64] = "";
	REQUIRE(parseDispatchRequestPayload(
		R"({"mode":"normal_mode","power_w":2500,"soc_percent":80,"duration_s":600})",
		payload,
		error,
		sizeof(error)));

	DispatchRequestPlan plan{};
	REQUIRE(buildDispatchRequestPlan(payload, plan, error, sizeof(error)));
	CHECK(plan.stop);
	CHECK(plan.matchStartStop);
	CHECK_FALSE(plan.matchMode);
	CHECK_FALSE(plan.matchPower);
	CHECK_FALSE(plan.matchSoc);
	CHECK_FALSE(plan.matchTime);

	DispatchRegisterReadback stopped{};
	stopped.dispatchStart = DISPATCH_START_STOP;
	CHECK(dispatchRequestReadbackMatches(plan, stopped, error, sizeof(error)));
}

TEST_CASE("dispatch request load following ignores soc and power in readback matching")
{
	DispatchRequestPayload payload{};
	char error[64] = "";
	REQUIRE(parseDispatchRequestPayload(
		R"({"mode":"load_following","power_w":1400,"soc_percent":55,"duration_s":90})",
		payload,
		error,
		sizeof(error)));

	DispatchRequestPlan plan{};
	REQUIRE(buildDispatchRequestPlan(payload, plan, error, sizeof(error)));
	CHECK(plan.dispatchMode == DISPATCH_MODE_LOAD_FOLLOWING);
	CHECK_FALSE(plan.matchPower);
	CHECK_FALSE(plan.matchSoc);

	DispatchRegisterReadback readback{};
	readback.dispatchStart = DISPATCH_START_START;
	readback.dispatchMode = DISPATCH_MODE_LOAD_FOLLOWING;
	readback.dispatchActivePower = DISPATCH_ACTIVE_POWER_OFFSET + 999;
	readback.dispatchSocRaw = 5;
	readback.dispatchTimeRaw = 90;
	CHECK(dispatchRequestReadbackMatches(plan, readback, error, sizeof(error)));
}

TEST_CASE("dispatch request reports specific mismatch strings")
{
	DispatchRequestPayload payload{};
	char error[64] = "";
	REQUIRE(parseDispatchRequestPayload(
		R"({"mode":"state_of_charge_control","power_w":3000,"soc_percent":30,"duration_s":45})",
		payload,
		error,
		sizeof(error)));

	DispatchRequestPlan plan{};
	REQUIRE(buildDispatchRequestPlan(payload, plan, error, sizeof(error)));

	DispatchRegisterReadback readback{};
	readback.dispatchStart = DISPATCH_START_START;
	readback.dispatchMode = plan.dispatchMode;
	readback.dispatchActivePower = plan.dispatchActivePower;
	readback.dispatchSocRaw = plan.dispatchSocRaw;
	readback.dispatchTimeRaw = plan.dispatchTimeRaw + 1;
	CHECK_FALSE(dispatchRequestReadbackMatches(plan, readback, error, sizeof(error)));
	CHECK(std::string(error) == "dispatch time mismatch");
}
