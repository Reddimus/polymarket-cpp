/// @file types.cpp
/// @brief Common type implementations

#include "polymarket/core/types.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <stdexcept>

#include "../detail/wide.hpp"
#include "polymarket/core/error.hpp"
#include "polymarket/crypto/keccak.hpp"

namespace polymarket {

namespace {

// Helper to parse hex character
std::uint8_t hex_char_to_nibble(char c) {
	if (c >= '0' && c <= '9')
		return static_cast<std::uint8_t>(c - '0');
	if (c >= 'a' && c <= 'f')
		return static_cast<std::uint8_t>(c - 'a' + 10);
	if (c >= 'A' && c <= 'F')
		return static_cast<std::uint8_t>(c - 'A' + 10);
	throw std::invalid_argument("Invalid hex character");
}

// Helper to parse hex string into bytes
void hex_to_bytes(std::string_view hex, std::span<std::uint8_t> out) {
	// Skip 0x prefix if present
	if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
		hex = hex.substr(2);
	}

	if (hex.empty() || hex.size() > out.size() * 2)
		throw std::invalid_argument("Invalid hex width");
	std::fill(out.begin(), out.end(), std::uint8_t{0});
	const std::size_t offset = out.size() * 2 - hex.size();
	for (std::size_t i = 0; i < hex.size(); ++i) {
		const std::uint8_t nibble = hex_char_to_nibble(hex[i]);
		const std::size_t position = offset + i;
		out[position / 2] |= static_cast<std::uint8_t>(nibble << (position % 2 ? 0 : 4));
	}
}

} // anonymous namespace

// uint256_t implementation

uint256_t uint256_t::from_string(std::string_view s) {
	if (s.empty())
		throw std::invalid_argument("Empty uint256");
	uint256_t result;
	for (const char c : s) {
		if (c < '0' || c > '9')
			throw std::invalid_argument("Invalid uint256 digit");
		unsigned carry = static_cast<unsigned>(c - '0');
		for (std::size_t i = result.bytes.size(); i-- > 0;) {
			const unsigned value = result.bytes[i] * 10U + carry;
			result.bytes[i] = static_cast<std::uint8_t>(value);
			carry = value >> 8;
		}
		if (carry)
			throw std::out_of_range("uint256 overflow");
	}
	return result;
}

uint256_t uint256_t::from_hex(std::string_view s) {
	uint256_t result;
	hex_to_bytes(s, result.bytes);
	return result;
}

std::string uint256_t::to_string() const {
	std::array<std::uint8_t, 32> remaining = bytes;
	std::string result;
	do {
		unsigned remainder = 0;
		for (std::uint8_t& byte : remaining) {
			const unsigned value = remainder * 256U + byte;
			byte = static_cast<std::uint8_t>(value / 10);
			remainder = value % 10;
		}
		result.push_back(static_cast<char>('0' + remainder));
	} while (
		std::any_of(remaining.begin(), remaining.end(), [](std::uint8_t b) { return b != 0; }));
	std::reverse(result.begin(), result.end());
	return result;
}

std::string uint256_t::to_decimal_string() const {
	// Both names return the full decimal representation.
	return to_string();
}

// Address implementation

Address Address::from_hex(std::string_view s) {
	Address result;
	hex_to_bytes(s, result.bytes);
	return result;
}

std::string Address::to_checksum() const {
	// EIP-55 checksum address implementation
	// 1. Get lowercase hex of address (without 0x prefix)
	std::string hex_addr;
	hex_addr.reserve(40);
	for (std::uint8_t b : bytes) {
		static const char hex_chars[] = "0123456789abcdef";
		hex_addr += hex_chars[(b >> 4) & 0xF];
		hex_addr += hex_chars[b & 0xF];
	}

	// 2. Hash the lowercase address string
	auto hash = crypto::keccak256(hex_addr);

	// 3. Apply checksum: uppercase if corresponding hash nibble >= 8
	std::string result = "0x";
	result.reserve(42);

	for (size_t i = 0; i < 40; ++i) {
		char c = hex_addr[i];
		if (c >= 'a' && c <= 'f') {
			// Get corresponding nibble from hash
			std::uint8_t hash_byte = hash[i / 2];
			std::uint8_t nibble = (i % 2 == 0) ? (hash_byte >> 4) : (hash_byte & 0xF);
			if (nibble >= 8) {
				c = static_cast<char>(std::toupper(c));
			}
		}
		result += c;
	}

	return result;
}

// Bytes32 implementation

Bytes32 Bytes32::from_hex(std::string_view s) {
	Bytes32 result;
	hex_to_bytes(s, result.bytes);
	return result;
}

// Decimal implementation

namespace {
std::uint64_t magnitude(std::int64_t value) {
	return value < 0 ? std::uint64_t{0} - static_cast<std::uint64_t>(value)
					 : static_cast<std::uint64_t>(value);
}
std::int64_t signed_value(std::uint64_t value, bool negative) {
	const std::uint64_t negative_limit = std::uint64_t{1} << 63;
	const std::uint64_t limit = negative ? negative_limit : negative_limit - 1;
	if (value > limit)
		throw std::overflow_error("Decimal overflow");
	if (negative && value == negative_limit)
		return std::numeric_limits<std::int64_t>::min();
	return negative ? -static_cast<std::int64_t>(value) : static_cast<std::int64_t>(value);
}
} // namespace

Decimal Decimal::from_string(std::string_view text) {
	bool negative = false;
	if (!text.empty() && (text.front() == '-' || text.front() == '+')) {
		negative = text.front() == '-';
		text.remove_prefix(1);
	}
	if (text.empty())
		throw std::invalid_argument("Empty decimal");
	const std::uint64_t limit = (std::uint64_t{1} << 63) - (negative ? 0 : 1);
	std::uint64_t scaled = 0;
	bool fraction = false, digit = false;
	int fractional_digits = 0;
	for (const char c : text) {
		if (c == '.' && !fraction) {
			fraction = true;
			continue;
		}
		if (c < '0' || c > '9')
			throw std::invalid_argument("Invalid decimal");
		digit = true;
		if (fraction && fractional_digits >= DECIMALS)
			continue;
		const unsigned next = static_cast<unsigned>(c - '0');
		if (scaled > (limit - next) / 10)
			throw std::out_of_range("Decimal overflow");
		scaled = scaled * 10 + next;
		if (fraction)
			++fractional_digits;
	}
	if (!digit)
		throw std::invalid_argument("Decimal requires digits");
	for (; fractional_digits < DECIMALS; ++fractional_digits) {
		if (scaled > limit / 10)
			throw std::out_of_range("Decimal overflow");
		scaled *= 10;
	}
	return Decimal(signed_value(scaled, negative));
}
Decimal Decimal::from_double(double value) {
	const double scaled = value * SCALE;
	if (!std::isfinite(scaled) || scaled < -9223372036854775808.0 ||
		scaled >= 9223372036854775808.0)
		throw std::out_of_range("Decimal overflow");
	return Decimal(static_cast<std::int64_t>(scaled));
}
Decimal Decimal::operator+(const Decimal& other) const {
	const std::int64_t b = other.value_;
	if ((b > 0 && value_ > std::numeric_limits<std::int64_t>::max() - b) ||
		(b < 0 && value_ < std::numeric_limits<std::int64_t>::min() - b))
		throw std::overflow_error("Decimal overflow");
	return Decimal(value_ + b);
}
Decimal Decimal::operator-(const Decimal& other) const {
	const std::int64_t b = other.value_;
	if ((b < 0 && value_ > std::numeric_limits<std::int64_t>::max() + b) ||
		(b > 0 && value_ < std::numeric_limits<std::int64_t>::min() + b))
		throw std::overflow_error("Decimal overflow");
	return Decimal(value_ - b);
}
Decimal Decimal::operator*(const Decimal& other) const {
	const bool negative = (value_ < 0) != (other.value_ < 0);
	const std::uint64_t limit = (std::uint64_t{1} << 63) - (negative ? 0 : 1);
	return Decimal(signed_value(
		detail::mul_div(magnitude(value_), magnitude(other.value_), SCALE, limit), negative));
}
Decimal Decimal::operator/(const Decimal& other) const {
	if (!other.value_)
		return Decimal{};
	const bool negative = (value_ < 0) != (other.value_ < 0);
	const std::uint64_t limit = (std::uint64_t{1} << 63) - (negative ? 0 : 1);
	return Decimal(signed_value(
		detail::mul_div(magnitude(value_), SCALE, magnitude(other.value_), limit), negative));
}
std::uint64_t Decimal::to_uint64_scaled(int decimals) const {
	if (value_ <= 0)
		return 0;
	if (decimals < 0 || decimals > 19)
		throw std::out_of_range("Decimal scale must be 0..19");
	std::uint64_t value = static_cast<std::uint64_t>(value_);
	for (int i = DECIMALS; i < decimals; ++i) {
		if (value > std::numeric_limits<std::uint64_t>::max() / 10)
			throw std::overflow_error("Decimal scale overflow");
		value *= 10;
	}
	for (int i = decimals; i < DECIMALS; ++i)
		value /= 10;
	return value;
}

} // namespace polymarket
