#pragma once

/// @file types.hpp
/// @brief CLOB API types and models
///
/// Defines all the request/response types for the Polymarket CLOB API.

#include "polymarket/core/types.hpp"
#include "polymarket/crypto/eip712.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace polymarket::clob {

// Re-export common types
using polymarket::Address;
using polymarket::Bytes32;
using polymarket::Decimal;
using polymarket::Timestamp;
using polymarket::uint256_t;
using polymarket::crypto::Order;
using polymarket::crypto::OrderType;
using polymarket::crypto::Side;
using polymarket::crypto::Signature;
using polymarket::crypto::SignatureType;

/// Tick size for price precision
enum class TickSize {
    Tenth,        ///< 0.1
    Hundredth,    ///< 0.01
    Thousandth,   ///< 0.001
    TenThousandth ///< 0.0001
};

/// Convert TickSize to decimal value
[[nodiscard]] inline Decimal to_decimal(TickSize tick) {
    switch (tick) {
        case TickSize::Tenth:        return Decimal::from_double(0.1);
        case TickSize::Hundredth:    return Decimal::from_double(0.01);
        case TickSize::Thousandth:   return Decimal::from_double(0.001);
        case TickSize::TenThousandth: return Decimal::from_double(0.0001);
    }
    return Decimal::from_double(0.01);
}

/// Order status
enum class OrderStatus {
    Live,
    Matched,
    Canceled,
    Delayed,
    Unmatched,
};

/// Trade status
enum class TradeStatus {
    Matched,
    Mined,
    Confirmed,
    Retrying,
    Failed,
};

/// Asset type for balance queries
enum class AssetType {
    Collateral,  ///< USDC
    Conditional, ///< Outcome tokens
};

/// Price interval for history queries
enum class Interval {
    OneMinute,  ///< 1m
    OneHour,    ///< 1h
    SixHours,   ///< 6h
    OneDay,     ///< 1d
    OneWeek,    ///< 1w
    Max,        ///< max
};

/// Convert Interval to API string
[[nodiscard]] inline std::string_view to_string(Interval interval) {
    switch (interval) {
        case Interval::OneMinute: return "1m";
        case Interval::OneHour:   return "1h";
        case Interval::SixHours:  return "6h";
        case Interval::OneDay:    return "1d";
        case Interval::OneWeek:   return "1w";
        case Interval::Max:       return "max";
    }
    return "1d";
}

/// Alias for interval string
[[nodiscard]] inline std::string to_interval_string(Interval interval) {
    return std::string(to_string(interval));
}

/// Convert OrderType to API string
[[nodiscard]] inline std::string to_order_type_string(OrderType type) {
    return std::string(crypto::to_string(type));
}

// =====================================================================
// Request Types
// =====================================================================

/// Request for single price
struct PriceRequest {
    uint256_t token_id;
    Side side;
};

/// Request for midpoint
struct MidpointRequest {
    uint256_t token_id;
};

/// Request for spread
struct SpreadRequest {
    uint256_t token_id;
    std::optional<Side> side;
};

/// Request for orderbook
struct OrderBookRequest {
    uint256_t token_id;
    std::optional<Side> side;
};

/// Request for price history
struct PriceHistoryRequest {
    uint256_t token_id;
    std::optional<Interval> interval;
    std::optional<Timestamp> start_ts;
    std::optional<Timestamp> end_ts;
    std::optional<std::uint32_t> fidelity;
};

/// Request for orders list
struct OrdersRequest {
    std::optional<std::string> order_id;
    std::optional<Bytes32> market;
    std::optional<uint256_t> asset_id;
};

/// Request for trades list
struct TradesRequest {
    std::optional<std::string> id;
    std::optional<Address> taker_address;
    std::optional<Address> maker_address;
    std::optional<Bytes32> market;
    std::optional<uint256_t> asset_id;
    std::optional<Timestamp> before;
    std::optional<Timestamp> after;
};

/// Request for balance/allowance
struct BalanceAllowanceRequest {
    AssetType asset_type{AssetType::Collateral};
    std::optional<uint256_t> token_id;
    std::optional<SignatureType> signature_type;
};

// =====================================================================
// Response Types
// =====================================================================

/// Server time response
struct ServerTimeResponse {
    Timestamp timestamp;
};

/// Health check response
struct HealthResponse {
    std::string status;  // "OK"
};

/// Price response
struct PriceResponse {
    Decimal price;
    uint256_t token_id;
    Side side;
};

/// Midpoint response
struct MidpointResponse {
    Decimal mid;
    uint256_t token_id;
};

/// Spread response
struct SpreadResponse {
    Decimal spread;
    uint256_t token_id;
};

/// Last trade price response
struct LastTradePriceResponse {
    Decimal price;
    uint256_t token_id;
    Timestamp timestamp;
};

/// Orderbook level
struct OrderBookLevel {
    Decimal price;
    Decimal size;
};

/// Orderbook response
struct OrderBookResponse {
    uint256_t token_id;
    std::vector<OrderBookLevel> bids;
    std::vector<OrderBookLevel> asks;
    Timestamp timestamp;
    std::string hash;  // Orderbook content hash
};

/// Tick size response
struct TickSizeResponse {
    uint256_t token_id;
    TickSize minimum_tick_size;
};

/// Neg risk response
struct NegRiskResponse {
    uint256_t token_id;
    bool neg_risk;
};

/// Fee rate response
struct FeeRateResponse {
    uint256_t token_id;
    std::uint32_t base_fee;  // Basis points
    std::uint32_t maker_fee; // Basis points (may be negative for rebate)
    std::uint32_t taker_fee; // Basis points
};

/// Geoblock response
struct GeoblockResponse {
    bool blocked;
    std::string country_code;
};

/// Market response
struct MarketResponse {
    std::string condition_id;
    uint256_t token_id;
    std::string question;
    std::string description;
    std::string market_slug;
    bool active;
    bool closed;
    bool neg_risk;
    TickSize minimum_tick_size;
    Decimal minimum_order_size;
    Timestamp end_date_iso;
    std::string icon;
};

/// API credentials
struct Credentials {
    std::string api_key;
    std::string api_secret;
    std::string passphrase;

    [[nodiscard]] bool valid() const noexcept {
        return !api_key.empty() && !api_secret.empty() && !passphrase.empty();
    }
};

/// Open order response
struct OpenOrderResponse {
    std::string id;
    std::string owner;
    uint256_t token_id;
    std::string market;
    Side side;
    Decimal price;
    Decimal original_size;
    Decimal size_matched;
    Decimal remaining_size;
    OrderType order_type;
    OrderStatus status;
    Timestamp created_at;
    Timestamp expiration;
    std::optional<bool> post_only;
};

/// Trade response
struct TradeResponse {
    std::string id;
    std::string taker_order_id;
    std::string market;
    uint256_t token_id;
    Side side;
    Decimal price;
    Decimal size;
    std::string maker_address;
    TradeStatus status;
    Timestamp match_time;
    std::optional<std::string> transaction_hash;
};

/// Order placement response
struct PostOrderResponse {
    std::string order_id;
    bool success;
    std::optional<std::string> error_msg;
    std::string status;
    std::optional<std::string> transaction_hash;
};

/// Cancel orders response
struct CancelOrdersResponse {
    std::vector<std::string> canceled;
    std::vector<std::string> not_canceled;
};

/// Balance/allowance response
struct BalanceAllowanceResponse {
    Decimal balance;
    Decimal allowance;
};

/// Notification response
struct NotificationResponse {
    std::string id;
    std::string type;
    std::string message;
    Timestamp created_at;
    bool read;
};

/// Order scoring response
struct OrderScoringResponse {
    std::string order_id;
    bool scoring;
};

/// Heartbeat response
struct HeartbeatResponse {
    std::string heartbeat_id;
    Timestamp timestamp;
};

/// Price history point
struct PriceHistoryPoint {
    Timestamp timestamp;
    Decimal price;
};

/// Price history response
struct PriceHistoryResponse {
    uint256_t token_id;
    std::vector<PriceHistoryPoint> history;
};

// =====================================================================
// Signed Order Types
// =====================================================================

/// Signable order (before signing)
struct SignableOrder {
    Order order;
    OrderType order_type{OrderType::GTC};
    std::optional<bool> post_only;
};

/// Signed order (ready for submission)
struct SignedOrder {
    Order order;
    Signature signature;
    OrderType order_type{OrderType::GTC};
    std::string owner;  // API key
    std::optional<bool> post_only;
};

} // namespace polymarket::clob
