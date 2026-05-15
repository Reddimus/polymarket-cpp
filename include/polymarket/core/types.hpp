#pragma once

/// @file types.hpp
/// @brief Common types used throughout the SDK
///
/// Includes fixed-precision decimal, big integers (uint256), and address types.

#include <array>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace polymarket {

/// 256-bit unsigned integer for token IDs and amounts
/// Stored as big-endian byte array for easy hex conversion
struct uint256_t {
	std::array<std::uint8_t, 32> bytes{};

	uint256_t() = default;

	/// Construct from a decimal string
	static uint256_t from_string(std::string_view s);

	/// Construct from hex string (with or without 0x prefix)
	static uint256_t from_hex(std::string_view s);

	/// Construct from uint64
	static uint256_t from_u64(std::uint64_t value) {
		uint256_t result;
		// Store in big-endian
		for (int i = 7; i >= 0; --i) {
			result.bytes[31 - i] = static_cast<std::uint8_t>(value >> (i * 8));
		}
		return result;
	}

	/// Alias for from_u64
	static uint256_t from_uint64(std::uint64_t value) { return from_u64(value); }

	/// Convert to uint64 (lower 64 bits)
	[[nodiscard]] std::uint64_t to_uint64() const noexcept {
		std::uint64_t result = 0;
		for (int i = 0; i < 8; ++i) {
			result = (result << 8) | bytes[24 + i];
		}
		return result;
	}

	/// Convert to decimal string
	[[nodiscard]] std::string to_decimal_string() const;

	/// Convert to hex string (with 0x prefix)
	[[nodiscard]] std::string to_hex() const {
		std::ostringstream oss;
		oss << "0x";
		for (auto b : bytes) {
			oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
		}
		return oss.str();
	}

	/// Convert to decimal string
	[[nodiscard]] std::string to_string() const;

	/// Check if zero
	[[nodiscard]] bool is_zero() const noexcept {
		for (auto b : bytes) {
			if (b != 0)
				return false;
		}
		return true;
	}

	bool operator==(const uint256_t& other) const = default;
	bool operator!=(const uint256_t& other) const = default;
};

/// Ethereum address (20 bytes)
struct Address {
	std::array<std::uint8_t, 20> bytes{};

	Address() = default;

	/// Construct from hex string (with or without 0x prefix)
	static Address from_hex(std::string_view s);

	/// Convert to checksum address (EIP-55)
	[[nodiscard]] std::string to_checksum() const;

	/// Convert to lowercase hex (with 0x prefix)
	[[nodiscard]] std::string to_hex() const {
		std::ostringstream oss;
		oss << "0x";
		for (auto b : bytes) {
			oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
		}
		return oss.str();
	}

	/// Get the size in bytes
	[[nodiscard]] static constexpr std::size_t size() noexcept { return 20; }

	/// Check if zero address
	[[nodiscard]] bool is_zero() const noexcept {
		for (auto b : bytes) {
			if (b != 0)
				return false;
		}
		return true;
	}

	bool operator==(const Address& other) const = default;
	bool operator!=(const Address& other) const = default;

	/// Zero address constant
	static const Address ZERO;
};

/// 32-byte hash (used for condition IDs, transaction hashes, etc.)
struct Bytes32 {
	std::array<std::uint8_t, 32> bytes{};

	Bytes32() = default;

	/// Construct from hex string (with or without 0x prefix)
	static Bytes32 from_hex(std::string_view s);

	/// Convert to hex string (with 0x prefix)
	[[nodiscard]] std::string to_hex() const {
		std::ostringstream oss;
		oss << "0x";
		for (auto b : bytes) {
			oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
		}
		return oss.str();
	}

	bool operator==(const Bytes32& other) const = default;
	bool operator!=(const Bytes32& other) const = default;
};

/// Fixed-point decimal with 6 decimal places (USDC precision)
/// Stored as int64 with implicit 10^6 denominator
class Decimal {
public:
	static constexpr int DECIMALS = 6;
	static constexpr std::int64_t SCALE = 1'000'000;

	Decimal() = default;

	/// Construct from raw scaled value
	explicit constexpr Decimal(std::int64_t raw) noexcept : value_(raw) {}

	/// Construct from string like "123.456"
	explicit Decimal(std::string_view s) : Decimal(from_string(s)) {}
	explicit Decimal(const char* s) : Decimal(std::string_view(s)) {}

	/// Construct from double (will truncate to 6 decimal places)
	static Decimal from_double(double d) { return Decimal(static_cast<std::int64_t>(d * SCALE)); }

	/// Construct from string like "123.456"
	static Decimal from_string(std::string_view s);

	/// Get raw scaled value
	[[nodiscard]] constexpr std::int64_t raw() const noexcept { return value_; }

	/// Convert to double
	[[nodiscard]] double to_double() const noexcept { return static_cast<double>(value_) / SCALE; }

	/// Convert to string
	[[nodiscard]] std::string to_string() const {
		std::int64_t integer = value_ / SCALE;
		std::int64_t fraction = value_ % SCALE;
		if (fraction < 0)
			fraction = -fraction;

		// If fraction is 0, return just the integer part
		if (fraction == 0) {
			return std::to_string(integer);
		}

		std::ostringstream oss;
		oss << integer << "." << std::setw(DECIMALS) << std::setfill('0') << fraction;
		std::string result = oss.str();

		// Strip trailing zeros after decimal point
		auto last_nonzero = result.find_last_not_of('0');
		if (last_nonzero != std::string::npos && result[last_nonzero] != '.') {
			result = result.substr(0, last_nonzero + 1);
		}
		return result;
	}

	/// Check if zero
	[[nodiscard]] constexpr bool is_zero() const noexcept { return value_ == 0; }

	/// Check if negative
	[[nodiscard]] constexpr bool is_negative() const noexcept { return value_ < 0; }

	// Arithmetic operators
	Decimal operator+(const Decimal& other) const { return Decimal(value_ + other.value_); }
	Decimal operator-(const Decimal& other) const { return Decimal(value_ - other.value_); }
	Decimal operator*(const Decimal& other) const {
		// Avoid overflow by dividing after multiplication
		return Decimal((value_ * other.value_) / SCALE);
	}
	Decimal operator/(const Decimal& other) const {
		return Decimal((value_ * SCALE) / other.value_);
	}

	// Comparison operators
	bool operator==(const Decimal& other) const = default;
	auto operator<=>(const Decimal& other) const = default;

	/// Convert to uint64 with specified decimal places
	/// e.g., to_uint64_scaled(6) converts 1.5 to 1500000
	[[nodiscard]] std::uint64_t to_uint64_scaled(int decimals) const {
		if (decimals == DECIMALS) {
			return static_cast<std::uint64_t>(value_);
		} else if (decimals > DECIMALS) {
			// Scale up
			int diff = decimals - DECIMALS;
			std::int64_t multiplier = 1;
			for (int i = 0; i < diff; ++i)
				multiplier *= 10;
			return static_cast<std::uint64_t>(value_ * multiplier);
		} else {
			// Scale down
			int diff = DECIMALS - decimals;
			std::int64_t divisor = 1;
			for (int i = 0; i < diff; ++i)
				divisor *= 10;
			return static_cast<std::uint64_t>(value_ / divisor);
		}
	}

	// Constants
	static const Decimal ZERO;
	static const Decimal ONE;

private:
	std::int64_t value_{0};
};

inline const Decimal Decimal::ZERO{static_cast<std::int64_t>(0)};
inline const Decimal Decimal::ONE{Decimal::SCALE};
inline const Address Address::ZERO{};

/// Timestamp in milliseconds since Unix epoch
using Timestamp = std::int64_t;

/// Get current timestamp in milliseconds
[[nodiscard]] inline Timestamp now_ms() {
	return std::chrono::duration_cast<std::chrono::milliseconds>(
			   std::chrono::system_clock::now().time_since_epoch())
		.count();
}

/// Get current timestamp in seconds
[[nodiscard]] inline std::int64_t now_seconds() {
	return std::chrono::duration_cast<std::chrono::seconds>(
			   std::chrono::system_clock::now().time_since_epoch())
		.count();
}

} // namespace polymarket
