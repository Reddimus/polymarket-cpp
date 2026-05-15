/// @file types.cpp
/// @brief Common type implementations

#include "polymarket/core/types.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>

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

	// Pad with leading zeros if needed
	size_t expected_chars = out.size() * 2;
	size_t offset = 0;
	if (hex.size() < expected_chars) {
		offset = expected_chars - hex.size();
		std::fill(out.begin(), out.begin() + offset / 2, std::uint8_t{0});
	}

	for (size_t i = 0; i < hex.size(); i += 2) {
		std::uint8_t high = hex_char_to_nibble(hex[i]);
		std::uint8_t low = (i + 1 < hex.size()) ? hex_char_to_nibble(hex[i + 1]) : 0;
		out[(i + offset) / 2] = (high << 4) | low;
	}
}

} // anonymous namespace

// uint256_t implementation

uint256_t uint256_t::from_string(std::string_view s) {
	// Simple decimal string parsing for small values
	// For production, use a proper big integer library
	uint256_t result;
	std::uint64_t value = 0;
	for (char c : s) {
		if (c >= '0' && c <= '9') {
			value = value * 10 + (c - '0');
		}
	}
	return from_u64(value);
}

uint256_t uint256_t::from_hex(std::string_view s) {
	uint256_t result;
	hex_to_bytes(s, result.bytes);
	return result;
}

std::string uint256_t::to_string() const {
	// Simple conversion for small values that fit in uint64
	// For full uint256 support, use a proper big integer library
	std::uint64_t value = 0;
	for (int i = 24; i < 32; ++i) {
		value = (value << 8) | bytes[i];
	}
	return std::to_string(value);
}

std::string uint256_t::to_decimal_string() const {
	// Alias for to_string() - both return decimal representation
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

Decimal Decimal::from_string(std::string_view s) {
	// Parse string like "123.456789"
	std::int64_t integer_part = 0;
	std::int64_t fraction_part = 0;
	int fraction_digits = 0;
	bool in_fraction = false;
	bool negative = false;

	for (size_t i = 0; i < s.size(); ++i) {
		char c = s[i];
		if (c == '-' && i == 0) {
			negative = true;
		} else if (c == '.') {
			in_fraction = true;
		} else if (c >= '0' && c <= '9') {
			if (in_fraction) {
				if (fraction_digits < DECIMALS) {
					fraction_part = fraction_part * 10 + (c - '0');
					++fraction_digits;
				}
			} else {
				integer_part = integer_part * 10 + (c - '0');
			}
		}
	}

	// Pad fraction to DECIMALS places
	while (fraction_digits < DECIMALS) {
		fraction_part *= 10;
		++fraction_digits;
	}

	std::int64_t result = integer_part * SCALE + fraction_part;
	return Decimal(negative ? -result : result);
}

} // namespace polymarket
