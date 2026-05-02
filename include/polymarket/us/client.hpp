/// @file us/client.hpp
/// @brief Polymarket US API client for USA event trading

#pragma once

#include "polymarket/core/error.hpp"
#include "polymarket/core/types.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace polymarket::us {

/// Credentials for Polymarket US API (Ed25519)
struct Credentials {
    std::string key_id;       ///< API key ID
    std::string secret_key;   ///< Base64-encoded secret key
};

/// Filter for events
struct EventFilter {
    std::optional<bool> active;
    std::optional<int> limit;
    std::optional<std::string> cursor;
};

/// Filter for markets
struct MarketFilter {
    std::optional<std::string> event_id;
    std::optional<bool> active;
    std::optional<int> limit;
    std::optional<std::string> cursor;
};

/// Filter for orders
struct OrderFilter {
    std::optional<std::string> market_id;
    std::optional<std::string> status;
    std::optional<int> limit;
    std::optional<std::string> cursor;
};

/// Order request for Polymarket US
struct OrderRequest {
    std::string market_id;
    std::string side;         ///< "buy" or "sell"
    std::string type;         ///< "limit" or "market"
    Decimal price;            ///< Price (for limit orders)
    Decimal size;             ///< Size in shares
    std::optional<bool> post_only;
    std::optional<int64_t> expiration;
};

/// Polymarket US API client
///
/// The Polymarket US API is the CFTC-regulated US event-trading platform.
/// Uses Ed25519 authentication instead of EIP-712 (which the offshore
/// CLOB uses).
///
/// Two hosts:
/// - Authed (orders, positions, balances, activities):
///   https://api.polymarket.us
/// - Public (markets, books, settlement, tags, candles):
///   https://gateway.polymarket.us
///
/// Auth (verified against docs.polymarket.us 2026-05-02):
/// - Headers: X-PM-Access-Key, X-PM-Timestamp (unix ms),
///   X-PM-Signature (base64 Ed25519 sig)
/// - Canonical message: f"{timestamp}{method}{path}" (no separators,
///   body NOT signed). Tolerance ±30s.
/// - Secret format: base64-decoded yields 64 bytes (seed||pub);
///   first 32 bytes is the Ed25519 seed.
class Client {
public:
    /// Default authed host
    static constexpr std::string_view kAuthedHost = "https://api.polymarket.us";
    /// Default public host
    static constexpr std::string_view kPublicHost = "https://gateway.polymarket.us";

    /// Create a new Polymarket US client
    /// @param authed_host Authed REST host (default: api.polymarket.us)
    /// @param public_host Public REST host (default: gateway.polymarket.us)
    explicit Client(std::string_view authed_host = kAuthedHost,
                    std::string_view public_host = kPublicHost);
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    Client(Client&&) noexcept;
    Client& operator=(Client&&) noexcept;

    /// Set API credentials for authenticated endpoints
    Result<void> set_credentials(Credentials creds);

    // ----- Public Endpoints -----

    /// List events
    [[nodiscard]] Result<std::string> get_events(const EventFilter& filter = {});

    /// Get event by ID
    [[nodiscard]] Result<std::string> get_event(std::string_view event_id);

    /// List markets
    [[nodiscard]] Result<std::string> get_markets(const MarketFilter& filter = {});

    /// Get market by ID
    [[nodiscard]] Result<std::string> get_market(std::string_view market_id);

    /// Get orderbook for a market
    [[nodiscard]] Result<std::string> get_orderbook(std::string_view market_id);

    // ----- Series -----

    /// List all series
    [[nodiscard]] Result<std::string> get_series();

    /// Get series by ticker
    [[nodiscard]] Result<std::string> get_series_by_ticker(std::string_view ticker);

    // ----- Sports -----

    /// List all sports
    [[nodiscard]] Result<std::string> get_sports();

    /// Get sport by ID
    [[nodiscard]] Result<std::string> get_sport(std::string_view sport_id);

    // ----- Search -----

    /// Search events/markets
    [[nodiscard]] Result<std::string> search(std::string_view query);

    // ----- Authenticated Endpoints -----

    /// Place an order
    [[nodiscard]] Result<std::string> place_order(const OrderRequest& request);

    /// Cancel an order
    [[nodiscard]] Result<void> cancel_order(std::string_view order_id);

    /// List orders
    [[nodiscard]] Result<std::string> get_orders(const OrderFilter& filter = {});

    /// Get positions
    [[nodiscard]] Result<std::string> get_positions();

    /// Get balance
    [[nodiscard]] Result<std::string> get_balance();

    /// Get account info
    [[nodiscard]] Result<std::string> get_account();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace polymarket::us
