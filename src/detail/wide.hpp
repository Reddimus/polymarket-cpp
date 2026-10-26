#pragma once
#include <cstdint>
#include <limits>
#include <stdexcept>
#if defined(_MSC_VER) && defined(_M_X64)
#include <intrin.h>
#endif

namespace polymarket::detail {
// Exact floor(a*b/divisor), including platforms without a native 128-bit type.
inline std::uint64_t mul_div(std::uint64_t a, std::uint64_t b, std::uint64_t divisor,
							 std::uint64_t limit = std::numeric_limits<std::uint64_t>::max()) {
	if (!divisor)
		throw std::domain_error("Division by zero");
#if defined(__SIZEOF_INT128__)
	const __uint128_t quotient = static_cast<__uint128_t>(a) * b / divisor;
	if (quotient > limit)
		throw std::overflow_error("Fixed-point overflow");
	return static_cast<std::uint64_t>(quotient);
#else
	std::uint64_t high, low;
#if defined(_MSC_VER) && defined(_M_X64)
	low = _umul128(a, b, &high);
#else
	constexpr std::uint64_t mask = 0xffffffffULL;
	const std::uint64_t w0 = (a & mask) * (b & mask);
	const std::uint64_t t = (a >> 32) * (b & mask) + (w0 >> 32);
	const std::uint64_t w1 = (t & mask) + (a & mask) * (b >> 32);
	high = (a >> 32) * (b >> 32) + (t >> 32) + (w1 >> 32);
	low = (w1 << 32) | (w0 & mask);
#endif
	if (high >= divisor)
		throw std::overflow_error("Fixed-point overflow");
#if defined(_MSC_VER) && defined(_M_X64)
	std::uint64_t remainder;
	const std::uint64_t quotient = _udiv128(high, low, divisor, &remainder);
#else
	std::uint64_t remainder = high;
	std::uint64_t quotient = 0;
	for (int i = 63; i >= 0; --i) {
		const bool carry = (remainder >> 63) != 0;
		remainder = (remainder << 1) | ((low >> i) & 1);
		if (carry || remainder >= divisor) {
			remainder -= divisor;
			quotient |= std::uint64_t{1} << i;
		}
	}
#endif
	if (quotient > limit)
		throw std::overflow_error("Fixed-point overflow");
	return quotient;
#endif
}
} // namespace polymarket::detail
