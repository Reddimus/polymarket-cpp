#pragma once

/// @file client.hpp
/// @brief CLOB REST API client
///
/// Provides a clean API for interacting with Polymarket's CLOB API.
/// Authentication is managed internally - public methods work without auth,
/// authenticated methods require calling authenticate() first.

#include "polymarket/clob/order_builder.hpp"
#include "polymarket/clob/types.hpp"
#include "polymarket/core/error.hpp"
#include "polymarket/core/pagination.hpp"
#include "polymarket/core/rate_limit.hpp"
#include "polymarket/core/retry.hpp"
#include "polymarket/crypto/eip712.hpp"
#include "polymarket/crypto/hmac.hpp"
#include "polymarket/crypto/secp256k1.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace polymarket::clob {

// Forward declarations
class Client;
class OrderBuilderBase;

/// Client configuration
struct ClientConfig {
  std::string base_url{"https://clob.polymarket.com"};
  std::string geoblock_url{"https://polymarket.com"};
  std::uint64_t chain_id{137}; // Polygon mainnet
  std::chrono::seconds timeout{30};
  bool use_server_time{false};
  RetryPolicy retry_policy{};
  RateLimiter::Config rate_limit{};
};

/// API key credentials
struct ApiKeyCredentials {
  std::string api_key;
  std::string api_secret;
  std::string api_passphrase;
};

/// Builder credentials (for order attribution)
struct BuilderKeyCredentials {
  std::string api_key;
  std::string api_secret;
  std::string passphrase;
};

/// Main CLOB API client
///
/// Example usage:
/// ```cpp
/// auto client = Client::create();
///
/// // Public endpoint (no auth required)
/// auto health = client.ok();
///
/// // Authenticate with private key
/// client.authenticate(private_key);
///
/// // Now can use authenticated endpoints
/// auto orders = client.orders();
/// ```
class Client {
public:
  /// Create a client with default configuration
  static Result<Client> create(ClientConfig config = ClientConfig{});

  // =========================================================================
  // Authentication
  // =========================================================================

  /// Check if client is authenticated
  [[nodiscard]] bool is_authenticated() const noexcept;

  /// Check if client has builder credentials
  [[nodiscard]] bool is_builder() const noexcept;

  /// Authenticate using a private key (derives API key via L1 auth)
  /// @param signer Private key for signing
  /// @param funder Optional funder address (for proxy wallets)
  /// @param sig_type Signature type (EOA, Proxy, or GnosisSafe)
  Result<void> authenticate(const crypto::PrivateKey &signer,
                            std::optional<Address> funder = std::nullopt,
                            SignatureType sig_type = SignatureType::EOA);

  /// Authenticate using existing API credentials
  Result<void> authenticate(const ApiKeyCredentials &credentials,
                            const Address &signer_address,
                            std::optional<Address> funder = std::nullopt);

  /// Set builder credentials for order attribution
  Result<void>
  set_builder_credentials(const BuilderKeyCredentials &credentials);

  /// Clear authentication
  void deauthenticate();

  // =========================================================================
  // Public Endpoints (no auth required)
  // =========================================================================

  /// Health check
  [[nodiscard]] Result<HealthResponse> ok();

  /// Get server timestamp
  [[nodiscard]] Result<ServerTimeResponse> server_time();

  /// Get price for a token
  [[nodiscard]] Result<PriceResponse> price(const PriceRequest &request);

  /// Get prices for multiple tokens
  [[nodiscard]] Result<std::vector<PriceResponse>>
  prices(const std::vector<PriceRequest> &requests);

  /// Get midpoint for a token
  [[nodiscard]] Result<MidpointResponse>
  midpoint(const MidpointRequest &request);

  /// Get midpoints for multiple tokens
  [[nodiscard]] Result<std::vector<MidpointResponse>>
  midpoints(const std::vector<MidpointRequest> &requests);

  /// Get spread for a token
  [[nodiscard]] Result<SpreadResponse> spread(const SpreadRequest &request);

  /// Get spreads for multiple tokens
  [[nodiscard]] Result<std::vector<SpreadResponse>>
  spreads(const std::vector<SpreadRequest> &requests);

  /// Get last trade price for a token
  [[nodiscard]] Result<LastTradePriceResponse>
  last_trade_price(uint256_t token_id);

  /// Get orderbook for a token
  [[nodiscard]] Result<OrderBookResponse> book(const OrderBookRequest &request);

  /// Get orderbooks for multiple tokens
  [[nodiscard]] Result<std::vector<OrderBookResponse>>
  books(const std::vector<OrderBookRequest> &requests);

  /// Get tick size for a token
  [[nodiscard]] Result<TickSizeResponse> tick_size(uint256_t token_id);

  /// Get neg-risk flag for a token
  [[nodiscard]] Result<NegRiskResponse> neg_risk(uint256_t token_id);

  /// Get fee rate for a token
  [[nodiscard]] Result<FeeRateResponse> fee_rate_bps(uint256_t token_id);

  /// Get price history
  [[nodiscard]] Result<PriceHistoryResponse>
  price_history(const PriceHistoryRequest &request);

  /// Get markets with pagination
  [[nodiscard]] Result<PaginatedResponse<MarketResponse>>
  markets(std::optional<Cursor> cursor = std::nullopt);

  /// Check geoblock status
  [[nodiscard]] Result<GeoblockResponse> check_geoblock();

  // =========================================================================
  // Cache Management
  // =========================================================================

  /// Set tick size in cache
  void set_tick_size(uint256_t token_id, TickSize tick_size);

  /// Set neg-risk flag in cache
  void set_neg_risk(uint256_t token_id, bool neg_risk);

  /// Set fee rate in cache
  void set_fee_rate_bps(uint256_t token_id, std::uint32_t fee_rate);

  /// Clear all caches
  void invalidate_caches();

  // =========================================================================
  // Authenticated Endpoints (require auth)
  // =========================================================================

  /// Get API keys
  [[nodiscard]] Result<std::vector<ApiKeyCredentials>> api_keys();

  /// Delete current API key
  [[nodiscard]] VoidResult delete_api_key();

  /// Create a limit order builder
  [[nodiscard]] Result<LimitOrderBuilder> limit_order(uint256_t token_id);

  /// Create a market order builder
  [[nodiscard]] Result<MarketOrderBuilder> market_order(uint256_t token_id);

  /// Sign an order
  [[nodiscard]] Result<SignedOrder> sign_order(const crypto::PrivateKey &key,
                                               const SignableOrder &order);

  /// Post a signed order
  [[nodiscard]] Result<PostOrderResponse> post_order(const SignedOrder &order);

  /// Post multiple signed orders
  [[nodiscard]] Result<std::vector<PostOrderResponse>>
  post_orders(const std::vector<SignedOrder> &orders);

  /// Get an order by ID
  [[nodiscard]] Result<OpenOrderResponse> order(std::string_view order_id);

  /// Get orders with filters
  [[nodiscard]] Result<PaginatedResponse<OpenOrderResponse>>
  orders(const OrdersRequest &request = {},
         std::optional<Cursor> cursor = std::nullopt);

  /// Cancel an order by ID
  [[nodiscard]] Result<CancelOrdersResponse>
  cancel_order(std::string_view order_id);

  /// Cancel multiple orders
  [[nodiscard]] Result<CancelOrdersResponse>
  cancel_orders(const std::vector<std::string> &order_ids);

  /// Cancel all orders
  [[nodiscard]] Result<CancelOrdersResponse> cancel_all_orders();

  /// Cancel orders for a specific market
  [[nodiscard]] Result<CancelOrdersResponse>
  cancel_market_orders(std::optional<Bytes32> market = std::nullopt,
                       std::optional<uint256_t> asset_id = std::nullopt);

  /// Get trades with filters
  [[nodiscard]] Result<PaginatedResponse<TradeResponse>>
  trades(const TradesRequest &request = {},
         std::optional<Cursor> cursor = std::nullopt);

  /// Get balance and allowance
  [[nodiscard]] Result<BalanceAllowanceResponse>
  balance_allowance(const BalanceAllowanceRequest &request = {});

  /// Update allowance
  [[nodiscard]] VoidResult
  update_balance_allowance(const BalanceAllowanceRequest &request);

  /// Get notifications
  [[nodiscard]] Result<std::vector<NotificationResponse>> notifications();

  /// Delete notifications
  [[nodiscard]] VoidResult
  delete_notifications(const std::vector<std::string> &notification_ids);

  /// Check if an order is scoring
  [[nodiscard]] Result<OrderScoringResponse>
  is_order_scoring(std::string_view order_id);

  /// Check if orders are scoring
  [[nodiscard]] Result<std::vector<OrderScoringResponse>>
  are_orders_scoring(const std::vector<std::string> &order_ids);

  /// Check if in closed-only mode
  [[nodiscard]] Result<bool> closed_only_mode();

  // =========================================================================
  // Builder Endpoints (require builder credentials)
  // =========================================================================

  /// Post heartbeat
  [[nodiscard]] Result<HeartbeatResponse>
  post_heartbeat(std::optional<std::string> heartbeat_id = std::nullopt);

  /// Get builder API keys
  [[nodiscard]] Result<std::vector<BuilderKeyCredentials>> builder_api_keys();

  /// Revoke builder API key
  [[nodiscard]] VoidResult revoke_builder_api_key();

  // =========================================================================
  // Accessors
  // =========================================================================

  /// Get the API host URL
  [[nodiscard]] const std::string &host() const noexcept;

  /// Get configuration
  [[nodiscard]] const ClientConfig &config() const noexcept;

  /// Get signer address (if authenticated)
  [[nodiscard]] std::optional<Address> signer_address() const noexcept;

  /// Get funder address (if authenticated, may be same as signer)
  [[nodiscard]] std::optional<Address> funder_address() const noexcept;

  // Move-only
  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;
  Client(Client &&) noexcept;
  Client &operator=(Client &&) noexcept;
  ~Client();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;

  explicit Client(std::unique_ptr<Impl> impl);
};

} // namespace polymarket::clob
