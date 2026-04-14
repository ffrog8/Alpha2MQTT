// Purpose: Verify raw debug read payload parsing stays deterministic and safe.

#include <doctest/doctest.h>

#include "RawReadRequest.h"

TEST_CASE("raw read request parses preferred payload")
{
	RawReadRequest request{};
	REQUIRE(parseRawReadRequestPayload(R"({"register":21055,"bytes":4})", request));
	CHECK(request.hasRegister);
	CHECK(request.hasBytes);
	CHECK(request.requestedReg == 21055);
	CHECK(request.requestedBytes == 4);
}

TEST_CASE("raw read request parses legacy alias hex payload with optional prefix")
{
	RawReadRequest request{};
	REQUIRE(parseRawReadRequestPayload(R"({"registerAddress":"523F","dataBytes":"4"})", request));
	CHECK(request.hasRegister);
	CHECK(request.hasBytes);
	CHECK(request.requestedReg == 0x523F);
	CHECK(request.requestedBytes == 4);
}

TEST_CASE("raw read request ignores nested and string-spoofed key names")
{
	RawReadRequest request{};
	REQUIRE(parseRawReadRequestPayload(
		R"({"meta":{"register":1,"bytes":8},"note":"register=2 bytes=6","register":21055,"bytes":4})",
		request));
	CHECK(request.requestedReg == 21055);
	CHECK(request.requestedBytes == 4);
}

TEST_CASE("raw read request rejects odd and oversized byte counts")
{
	RawReadRequest oddRequest{};
	CHECK_FALSE(parseRawReadRequestPayload(R"({"register":21055,"bytes":3})", oddRequest));

	RawReadRequest oversizedRequest{};
	CHECK_FALSE(parseRawReadRequestPayload(R"({"register":21055,"bytes":60})", oversizedRequest));
}
