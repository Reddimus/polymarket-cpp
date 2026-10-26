#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>

// Standalone target: exercise the portable fallback against Python integer fixtures.
#ifdef __SIZEOF_INT128__
#undef __SIZEOF_INT128__
#endif
#include "../../src/detail/wide.hpp"

TEST_CASE("Wide multiply/divide agrees with arbitrary-precision fixtures", "[math]") {
	std::ifstream input(POLYMARKET_WIDE_FIXTURE);
	REQUIRE(input.good());
	std::uint64_t a, b, divisor;
	std::string expected;
	int count = 0;
	while (input >> a >> b >> divisor >> expected) {
		INFO("row " << count);
		if (expected == "overflow") {
			REQUIRE_THROWS_AS(polymarket::detail::mul_div(a, b, divisor), std::overflow_error);
		} else {
			REQUIRE(polymarket::detail::mul_div(a, b, divisor) == std::stoull(expected));
		}
		++count;
	}
	REQUIRE(input.eof());
	REQUIRE(count == 128);
	REQUIRE_THROWS_AS(polymarket::detail::mul_div(1, 1, 0), std::domain_error);
}
