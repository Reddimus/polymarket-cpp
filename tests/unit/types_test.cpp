/// @file types_test.cpp
/// @brief Unit tests for core types

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "polymarket/core/types.hpp"

using namespace polymarket;

TEST_CASE("uint256_t", "[types]") {
    SECTION("Default construction") {
        uint256_t value;
        REQUIRE(value.is_zero());
    }

    SECTION("From uint64") {
        auto value = uint256_t::from_uint64(12345);
        REQUIRE(!value.is_zero());
        REQUIRE(value.to_uint64() == 12345);
    }

    SECTION("From hex") {
        auto value = uint256_t::from_hex("0x1234");
        REQUIRE(value.to_uint64() == 0x1234);
    }

    SECTION("From hex without prefix") {
        auto value = uint256_t::from_hex("abcd");
        REQUIRE(value.to_uint64() == 0xabcd);
    }

    SECTION("To hex string") {
        auto value = uint256_t::from_uint64(255);
        std::string hex = value.to_hex();
        REQUIRE(hex.size() == 66);  // "0x" + 64 hex chars
        REQUIRE(hex.substr(0, 2) == "0x");
    }

    SECTION("To decimal string") {
        auto value = uint256_t::from_uint64(12345);
        REQUIRE(value.to_decimal_string() == "12345");
    }

    SECTION("Equality") {
        auto a = uint256_t::from_uint64(100);
        auto b = uint256_t::from_uint64(100);
        auto c = uint256_t::from_uint64(200);

        REQUIRE(a == b);
        REQUIRE(!(a == c));
    }

    SECTION("Large hex value") {
        // Max safe integer for JSON (2^53 - 1)
        auto value = uint256_t::from_uint64(9007199254740991ULL);
        REQUIRE(value.to_uint64() == 9007199254740991ULL);
    }
}

TEST_CASE("Address", "[types]") {
    SECTION("Default construction") {
        Address addr;
        REQUIRE(addr.to_checksum() == "0x0000000000000000000000000000000000000000");
    }

    SECTION("From hex with prefix") {
        auto addr = Address::from_hex("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed");
        REQUIRE(addr.bytes[0] == 0x5a);
    }

    SECTION("From hex without prefix") {
        auto addr = Address::from_hex("5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed");
        REQUIRE(addr.bytes[0] == 0x5a);
    }

    SECTION("Checksum encoding") {
        auto addr = Address::from_hex("0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed");
        // EIP-55 checksum
        REQUIRE(addr.to_checksum() == "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed");
    }

    SECTION("Equality") {
        auto a = Address::from_hex("0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed");
        auto b = Address::from_hex("0x5AAEB6053f3e94c9b9a09f33669435e7ef1beaed");
        
        REQUIRE(a == b);  // Case-insensitive comparison
    }
}

TEST_CASE("Bytes32", "[types]") {
    SECTION("Default construction") {
        Bytes32 b;
        for (auto byte : b.bytes) {
            REQUIRE(byte == 0);
        }
    }

    SECTION("From hex") {
        Bytes32 b = Bytes32::from_hex(
            "0x0000000000000000000000000000000000000000000000000000000000000001");
        REQUIRE(b.bytes[31] == 1);
    }

    SECTION("To hex") {
        Bytes32 b;
        b.bytes[31] = 0xff;
        std::string hex = b.to_hex();
        REQUIRE(hex.size() == 66);
        REQUIRE(hex.ends_with("ff"));
    }
}

TEST_CASE("Decimal", "[types]") {
    SECTION("Default construction") {
        Decimal d;
        REQUIRE(d.is_zero());
    }

    SECTION("From string") {
        Decimal d("123.456");
        REQUIRE(!d.is_zero());
        REQUIRE(d.to_string() == "123.456");
    }

    SECTION("From integer string") {
        Decimal d("100");
        REQUIRE(d.to_string() == "100");
    }

    SECTION("Comparison") {
        Decimal a("0.5");
        Decimal b("0.6");
        Decimal c("0.5");

        REQUIRE(a < b);
        REQUIRE(!(b < a));
        REQUIRE(!(a < c));  // Equal
    }

    SECTION("Scaled conversion") {
        Decimal d("1.5");
        // 1.5 with 6 decimals = 1500000
        auto scaled = d.to_uint64_scaled(6);
        REQUIRE(scaled == 1500000);
    }

    SECTION("Small decimals") {
        Decimal d("0.001");
        auto scaled = d.to_uint64_scaled(6);
        REQUIRE(scaled == 1000);  // 0.001 * 10^6 = 1000
    }

    SECTION("Price precision") {
        // Typical Polymarket price range
        Decimal price("0.523");
        auto scaled = price.to_uint64_scaled(6);
        REQUIRE(scaled == 523000);
    }
}
