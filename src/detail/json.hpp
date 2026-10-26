#pragma once
#include <cmath>
#include <cstdint>
#include <glaze/glaze.hpp>
#include <glaze/json/generic.hpp>
#include <limits>
#include <type_traits>
namespace polymarket::detail {
struct ReadOptions : glz::opts {
	bool validate_trailing_whitespace = true;
};
inline constexpr ReadOptions kReadOptions{{.null_terminated = false}};
template <class Integer>
bool exact_integer(const glz::generic_u64& value, Integer& out) {
	if (const std::uint64_t* number = value.get_if<std::uint64_t>()) {
		if (*number > static_cast<std::uint64_t>(std::numeric_limits<Integer>::max())) {
			return false;
		}
		out = static_cast<Integer>(*number);
		return true;
	}
	if (const std::int64_t* number = value.get_if<std::int64_t>()) {
		if constexpr (std::is_unsigned_v<Integer>) {
			if (*number < 0 ||
				static_cast<std::uint64_t>(*number) > std::numeric_limits<Integer>::max()) {
				return false;
			}
		} else if (*number < std::numeric_limits<Integer>::min() ||
				   *number > std::numeric_limits<Integer>::max()) {
			return false;
		}
		out = static_cast<Integer>(*number);
		return true;
	}
	if (const double* number = value.get_if<double>()) {
		const double upper = std::ldexp(1.0, std::numeric_limits<Integer>::digits);
		const double lower = std::is_unsigned_v<Integer> ? 0.0 : -upper;
		if (!std::isfinite(*number) || *number < lower || *number >= upper ||
			std::trunc(*number) != *number) {
			return false;
		}
		out = static_cast<Integer>(*number);
		return true;
	}
	return false;
}

} // namespace polymarket::detail
