#pragma once

/// @file order_builder.hpp
/// @brief Type-safe order builders for limit and market orders
///
/// Provides fluent builders that validate orders at build time,
/// matching the Rust SDK's OrderBuilder pattern.

#include "polymarket/clob/types.hpp"
#include "polymarket/core/error.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <random>

namespace polymarket::clob {

// Forward declaration
class Client;

/// Constants for order validation
namespace constants {
    constexpr std::uint32_t USDC_DECIMALS = 6;
    constexpr std::uint32_t LOT_SIZE_SCALE = 2;  // Max decimal places for size
    constexpr std::uint64_t IEEE754_MAX_SAFE = (1ULL << 53) - 1;  // For JSON number safety
}

/// Generate a random salt for orders
/// Masks to IEEE 754 safe integer range (≤ 2^53 - 1)
[[nodiscard]] inline std::uint64_t generate_salt() {
    static thread_local std::mt19937_64 rng{std::random_device{}()};
    return rng() & constants::IEEE754_MAX_SAFE;
}

/// Marker type for limit orders
struct Limit {};

/// Marker type for market orders  
struct Market {};

/// Type-safe order builder
///
/// Validates orders at build time to catch errors early.
/// Uses phantom type parameter to distinguish limit vs market orders.
///
/// @tparam OrderKind Either Limit or Market
template <typename OrderKind>
class OrderBuilder {
public:
    /// Set the token ID (required)
    OrderBuilder& token_id(uint256_t id) {
        token_id_ = std::move(id);
        return *this;
    }

    /// Set the order side (required)
    OrderBuilder& side(Side s) {
        side_ = s;
        return *this;
    }

    /// Set the nonce (optional, defaults to 0)
    OrderBuilder& nonce(std::uint64_t n) {
        nonce_ = n;
        return *this;
    }

    /// Set expiration timestamp (optional, only valid for GTD orders)
    OrderBuilder& expiration(Timestamp exp) {
        expiration_ = exp;
        return *this;
    }

    /// Set taker address (optional, defaults to zero address)
    OrderBuilder& taker(Address addr) {
        taker_ = std::move(addr);
        return *this;
    }

    /// Set order type (optional, defaults to GTC for limit, FAK for market)
    OrderBuilder& order_type(OrderType type) {
        order_type_ = type;
        return *this;
    }

    /// Set post-only flag (optional, only valid for GTC/GTD limit orders)
    OrderBuilder& post_only(bool po) {
        post_only_ = po;
        return *this;
    }

protected:
    friend class Client;

    // Protected constructor - use specializations' create() methods
    OrderBuilder(Address signer, Address maker, SignatureType sig_type,
                 TickSize tick_size, std::uint32_t fee_rate, bool neg_risk)
        : signer_(std::move(signer))
        , maker_(std::move(maker))
        , signature_type_(sig_type)
        , tick_size_(tick_size)
        , fee_rate_(fee_rate)
        , neg_risk_(neg_risk) {}

protected:

    std::optional<uint256_t> token_id_;
    std::optional<Side> side_;
    std::optional<Decimal> price_;
    std::optional<Decimal> size_;
    std::optional<std::uint64_t> nonce_;
    std::optional<Timestamp> expiration_;
    std::optional<Address> taker_;
    std::optional<OrderType> order_type_;
    std::optional<bool> post_only_;

    Address signer_;
    Address maker_;
    SignatureType signature_type_;
    TickSize tick_size_;
    std::uint32_t fee_rate_;
    bool neg_risk_;
};

/// Limit order builder
template <>
class OrderBuilder<Limit> : public OrderBuilder<void> {
public:
    /// Create a new limit order builder
    static OrderBuilder<Limit> create(Address signer, Address maker, SignatureType sig_type,
                                      TickSize tick_size, std::uint32_t fee_rate, bool neg_risk) {
        return OrderBuilder<Limit>(std::move(signer), std::move(maker), sig_type, tick_size, fee_rate, neg_risk);
    }

    /// Set the price (required for limit orders)
    OrderBuilder& price(Decimal p) {
        price_ = p;
        return *this;
    }

    /// Set the size in shares (required for limit orders)
    OrderBuilder& size(Decimal s) {
        size_ = s;
        return *this;
    }

    /// Build and validate the order
    [[nodiscard]] Result<SignableOrder> build() const;

private:
    friend class Client;
    using OrderBuilder<void>::OrderBuilder;
};

/// Market order builder
template <>
class OrderBuilder<Market> : public OrderBuilder<void> {
public:
    /// Create a new market order builder
    static OrderBuilder<Market> create(Address signer, Address maker, SignatureType sig_type,
                                       TickSize tick_size, std::uint32_t fee_rate, bool neg_risk) {
        return OrderBuilder<Market>(std::move(signer), std::move(maker), sig_type, tick_size, fee_rate, neg_risk);
    }

    /// Set the amount in USDC (for buy orders)
    OrderBuilder& amount_usdc(Decimal amount) {
        amount_ = amount;
        is_usdc_ = true;
        return *this;
    }

    /// Set the amount in shares (for buy or sell orders)
    OrderBuilder& amount_shares(Decimal amount) {
        amount_ = amount;
        is_usdc_ = false;
        return *this;
    }

    /// Set price hint (optional, will be calculated from orderbook if not provided)
    OrderBuilder& price(Decimal p) {
        price_ = p;
        return *this;
    }

    /// Build and validate the order
    [[nodiscard]] Result<SignableOrder> build() const;

private:
    friend class Client;
    using OrderBuilder<void>::OrderBuilder;
    std::optional<Decimal> amount_;
    bool is_usdc_{false};
};

/// Alias for convenience
using LimitOrderBuilder = OrderBuilder<Limit>;
using MarketOrderBuilder = OrderBuilder<Market>;

} // namespace polymarket::clob
