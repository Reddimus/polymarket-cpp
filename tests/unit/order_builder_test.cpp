/// @file order_builder_test.cpp
/// @brief Unit tests for order builders

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "polymarket/clob/order_builder.hpp"

using namespace polymarket;
using namespace polymarket::clob;

TEST_CASE("LimitOrderBuilder", "[clob][order]") {
  Address signer =
      Address::from_hex("0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266");
  Address maker = signer;
  auto token_id = uint256_t::from_uint64(12345);

  SECTION("Valid buy order") {
    auto builder = LimitOrderBuilder::create(signer, maker, SignatureType::EOA,
                                             TickSize::Hundredth, 0, false);
    builder.token_id(token_id);
    builder.side(Side::Buy);
    builder.price(Decimal("0.5"));
    builder.size(Decimal("10"));

    auto result = builder.build();
    REQUIRE(result.has_value());
    REQUIRE(result->order.side == 0); // BUY
    REQUIRE(result->order.maker == maker);
    REQUIRE(result->order.signer == signer);
  }

  SECTION("Valid sell order") {
    auto builder = LimitOrderBuilder::create(signer, maker, SignatureType::EOA,
                                             TickSize::Hundredth, 0, false);
    builder.token_id(token_id);
    builder.side(Side::Sell);
    builder.price(Decimal("0.6"));
    builder.size(Decimal("5"));

    auto result = builder.build();
    REQUIRE(result.has_value());
    REQUIRE(result->order.side == 1); // SELL
  }

  SECTION("Missing token_id") {
    auto builder = LimitOrderBuilder::create(signer, maker, SignatureType::EOA,
                                             TickSize::Hundredth, 0, false);
    builder.side(Side::Buy);
    builder.price(Decimal("0.5"));
    builder.size(Decimal("10"));

    auto result = builder.build();
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code() == ErrorCode::ValidationError);
  }

  SECTION("Missing price") {
    auto builder = LimitOrderBuilder::create(signer, maker, SignatureType::EOA,
                                             TickSize::Hundredth, 0, false);
    builder.token_id(token_id);
    builder.side(Side::Buy);
    builder.size(Decimal("10"));

    auto result = builder.build();
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code() == ErrorCode::ValidationError);
  }

  SECTION("Missing size") {
    auto builder = LimitOrderBuilder::create(signer, maker, SignatureType::EOA,
                                             TickSize::Hundredth, 0, false);
    builder.token_id(token_id);
    builder.side(Side::Buy);
    builder.price(Decimal("0.5"));

    auto result = builder.build();
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code() == ErrorCode::ValidationError);
  }

  SECTION("Missing side") {
    auto builder = LimitOrderBuilder::create(signer, maker, SignatureType::EOA,
                                             TickSize::Hundredth, 0, false);
    builder.token_id(token_id);
    builder.price(Decimal("0.5"));
    builder.size(Decimal("10"));

    auto result = builder.build();
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code() == ErrorCode::ValidationError);
  }

  SECTION("Price too low") {
    auto builder = LimitOrderBuilder::create(signer, maker, SignatureType::EOA,
                                             TickSize::Hundredth, 0, false);
    builder.token_id(token_id);
    builder.side(Side::Buy);
    builder.price(Decimal("0.0001")); // Below 0.001
    builder.size(Decimal("10"));

    auto result = builder.build();
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code() == ErrorCode::ValidationError);
  }

  SECTION("Price too high") {
    auto builder = LimitOrderBuilder::create(signer, maker, SignatureType::EOA,
                                             TickSize::Hundredth, 0, false);
    builder.token_id(token_id);
    builder.side(Side::Buy);
    builder.price(Decimal("0.9999")); // Above 0.999
    builder.size(Decimal("10"));

    auto result = builder.build();
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code() == ErrorCode::ValidationError);
  }
}

TEST_CASE("MarketOrderBuilder", "[clob][order]") {
  Address signer =
      Address::from_hex("0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266");
  Address maker = signer;
  auto token_id = uint256_t::from_uint64(12345);

  SECTION("Valid buy order with USDC amount") {
    auto builder = MarketOrderBuilder::create(signer, maker, SignatureType::EOA,
                                              TickSize::Hundredth, 0, false);
    builder.token_id(token_id);
    builder.side(Side::Buy);
    builder.amount_usdc(Decimal("10"));

    auto result = builder.build();
    REQUIRE(result.has_value());
    REQUIRE(result->order.side == 0); // BUY
  }

  SECTION("Valid sell order with shares amount") {
    auto builder = MarketOrderBuilder::create(signer, maker, SignatureType::EOA,
                                              TickSize::Hundredth, 0, false);
    builder.token_id(token_id);
    builder.side(Side::Sell);
    builder.amount_shares(Decimal("100"));

    auto result = builder.build();
    REQUIRE(result.has_value());
    REQUIRE(result->order.side == 1); // SELL
  }

  SECTION("Buy with shares amount") {
    auto builder = MarketOrderBuilder::create(signer, maker, SignatureType::EOA,
                                              TickSize::Hundredth, 0, false);
    builder.token_id(token_id);
    builder.side(Side::Buy);
    builder.amount_shares(Decimal("10"));

    auto result = builder.build();
    REQUIRE(result.has_value());
  }

  SECTION("Missing amount") {
    auto builder = MarketOrderBuilder::create(signer, maker, SignatureType::EOA,
                                              TickSize::Hundredth, 0, false);
    builder.token_id(token_id);
    builder.side(Side::Buy);

    auto result = builder.build();
    REQUIRE(!result.has_value());
    REQUIRE(result.error().code() == ErrorCode::ValidationError);
  }
}

TEST_CASE("Salt generation", "[clob][order]") {
  // Salt should be <= 2^53 - 1 for IEEE 754 JSON safety
  constexpr std::uint64_t IEEE754_MAX_SAFE = (1ULL << 53) - 1;

  for (int i = 0; i < 100; ++i) {
    auto salt = generate_salt();
    REQUIRE(salt <= IEEE754_MAX_SAFE);
  }
}
