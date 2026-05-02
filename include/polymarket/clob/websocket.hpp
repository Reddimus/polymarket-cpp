#pragma once

/// @file websocket.hpp
/// @brief WebSocket client for real-time CLOB market data and user events
///
/// Provides subscription-based streaming for:
/// - Market channel: orderbook updates, price changes, trades
/// - User channel: order fills, cancellations (authenticated)

#include "polymarket/clob/types.hpp"
#include "polymarket/core/error.hpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace polymarket::clob {

// =========================================================================
// WebSocket Message Types
// =========================================================================

/// Orderbook snapshot message
struct BookSnapshot {
  uint256_t asset_id;
  std::vector<OrderBookLevel> bids;
  std::vector<OrderBookLevel> asks;
  Timestamp timestamp;
  std::string hash;
};

/// Orderbook delta (incremental update)
struct BookDelta {
  uint256_t asset_id;
  Decimal price;
  Decimal size; // New size at price level (0 = level removed)
  Side side;
  Timestamp timestamp;
};

/// Price change message
struct PriceChange {
  uint256_t asset_id;
  Decimal price;
  Side side;
  Timestamp timestamp;
};

/// Last trade price message
struct LastTradePriceMessage {
  uint256_t asset_id;
  Decimal price;
  Timestamp timestamp;
};

/// Tick size change message
struct TickSizeChange {
  uint256_t asset_id;
  TickSize new_tick_size;
  Timestamp timestamp;
};

/// Best bid/ask message (custom features)
struct BestBidAsk {
  uint256_t asset_id;
  std::optional<Decimal> best_bid;
  std::optional<Decimal> best_ask;
  Timestamp timestamp;
};

/// New market message (custom features)
struct NewMarket {
  std::vector<uint256_t> asset_ids;
  std::string condition_id;
  Timestamp timestamp;
};

/// Market resolved message (custom features)
struct MarketResolved {
  std::vector<uint256_t> asset_ids;
  std::string condition_id;
  std::string winner; // Outcome that won
  Timestamp timestamp;
};

/// Order fill message (user channel)
struct OrderFill {
  std::string order_id;
  uint256_t asset_id;
  Decimal price;
  Decimal size;
  Side side;
  bool is_taker;
  Timestamp timestamp;
  std::optional<std::string> transaction_hash;
};

/// Order cancel message (user channel)
struct OrderCancel {
  std::string order_id;
  uint256_t asset_id;
  std::string reason;
  Timestamp timestamp;
};

/// Trade confirmation message (user channel)
struct TradeConfirm {
  std::string trade_id;
  std::string order_id;
  uint256_t asset_id;
  Decimal price;
  Decimal size;
  TradeStatus status;
  Timestamp timestamp;
};

/// Union of all WebSocket message types
using WsMessage =
    std::variant<BookSnapshot, BookDelta, PriceChange, LastTradePriceMessage,
                 TickSizeChange, BestBidAsk, NewMarket, MarketResolved,
                 OrderFill, OrderCancel, TradeConfirm>;

/// WebSocket error
struct WsError {
  int code{0};
  std::string message;
};

// =========================================================================
// Callback Types
// =========================================================================

/// Callback for WebSocket messages
using WsMessageCallback = std::function<void(const WsMessage &)>;

/// Callback for WebSocket errors
using WsErrorCallback = std::function<void(const WsError &)>;

/// Callback for connection state changes
using WsStateCallback = std::function<void(bool connected)>;

// =========================================================================
// WebSocket Configuration
// =========================================================================

/// WebSocket client configuration
struct WsConfig {
  std::string url{"wss://ws-subscriptions-clob.polymarket.com/ws/"};
  std::chrono::seconds reconnect_delay{5};
  std::uint16_t max_reconnect_attempts{10};
  bool auto_reconnect{true};
  std::chrono::seconds ping_interval{30};
  bool enable_custom_features{
      false}; // Enables best_bid_ask, new_market, market_resolved
};

/// Subscription ID for tracking active subscriptions
struct SubscriptionId {
  std::uint64_t id{0};

  [[nodiscard]] bool valid() const noexcept { return id != 0; }
};

/// Channel type
enum class ChannelType {
  Market, ///< Public market data
  User,   ///< Authenticated user events
};

// =========================================================================
// WebSocket Client
// =========================================================================

/// WebSocket streaming client for real-time CLOB data
///
/// Supports both market (public) and user (authenticated) channels
/// with automatic reconnection and subscription management.
class WebSocketClient {
public:
  /// Create a WebSocket client for public market data
  explicit WebSocketClient(WsConfig config = {});

  /// Create a WebSocket client with authentication for user channel
  WebSocketClient(WsConfig config, Credentials credentials);

  ~WebSocketClient();

  // Non-copyable but moveable
  WebSocketClient(const WebSocketClient &) = delete;
  WebSocketClient &operator=(const WebSocketClient &) = delete;
  WebSocketClient(WebSocketClient &&) noexcept;
  WebSocketClient &operator=(WebSocketClient &&) noexcept;

  // =========================================================================
  // Connection Management
  // =========================================================================

  /// Connect to the WebSocket server
  [[nodiscard]] VoidResult connect();

  /// Disconnect from the server
  void disconnect();

  /// Check if connected
  [[nodiscard]] bool is_connected() const noexcept;

  // =========================================================================
  // Market Channel (Public)
  // =========================================================================

  /// Subscribe to market data for specific assets
  /// @param asset_ids Token IDs to subscribe to
  /// @return Subscription ID for later unsubscription
  [[nodiscard]] Result<SubscriptionId>
  subscribe_market(const std::vector<uint256_t> &asset_ids);

  /// Subscribe to market data with custom features enabled
  [[nodiscard]] Result<SubscriptionId>
  subscribe_market_with_options(const std::vector<uint256_t> &asset_ids,
                                bool custom_features = false);

  /// Unsubscribe from market data
  [[nodiscard]] VoidResult
  unsubscribe_market(const std::vector<uint256_t> &asset_ids);

  // =========================================================================
  // User Channel (Authenticated)
  // =========================================================================

  /// Subscribe to user events for specific markets
  /// @param markets Condition IDs to subscribe to (empty = all markets)
  [[nodiscard]] Result<SubscriptionId>
  subscribe_user(const std::vector<Bytes32> &markets = {});

  /// Unsubscribe from user events
  [[nodiscard]] VoidResult
  unsubscribe_user(const std::vector<Bytes32> &markets);

  // =========================================================================
  // Callbacks
  // =========================================================================

  /// Set callback for incoming messages
  void on_message(WsMessageCallback callback);

  /// Set callback for errors
  void on_error(WsErrorCallback callback);

  /// Set callback for connection state changes
  void on_state_change(WsStateCallback callback);

  // =========================================================================
  // Accessors
  // =========================================================================

  /// Get the configuration
  [[nodiscard]] const WsConfig &config() const noexcept;

  /// Get the number of active subscriptions
  [[nodiscard]] std::size_t subscription_count() const noexcept;

  /// Check if authenticated (can use user channel)
  [[nodiscard]] bool is_authenticated() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace polymarket::clob
