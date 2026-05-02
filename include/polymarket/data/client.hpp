/// @file data/client.hpp
/// @brief Data API client for positions and trades

#pragma once

#include "polymarket/core/error.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace polymarket::data {

/// Filter for listing trades
struct TradeFilter {
  std::optional<std::string> market;
  std::optional<std::string> maker;
  std::optional<std::string> taker;
  std::optional<int64_t> before;
  std::optional<int64_t> after;
  std::optional<int> limit;
  std::optional<std::string> cursor;
};

/// Data API client for positions and trade history
///
/// The Data API provides:
/// - User positions (with redeemable/mergeable flags)
/// - User activity
/// - Trade history
/// - Leaderboard data
///
/// Base URL: https://data-api.polymarket.com
class Client {
public:
  /// Create a new Data API client
  /// @param base_url Base URL (default: https://data-api.polymarket.com)
  explicit Client(
      std::string_view base_url = "https://data-api.polymarket.com");
  ~Client();

  Client(const Client &) = delete;
  Client &operator=(const Client &) = delete;
  Client(Client &&) noexcept;
  Client &operator=(Client &&) noexcept;

  // ----- Positions -----

  /// Get positions for an address
  [[nodiscard]] Result<std::string> get_positions(std::string_view address);

  // ----- Activity -----

  /// Get activity for an address
  [[nodiscard]] Result<std::string> get_activity(std::string_view address);

  // ----- Trades -----

  /// Get trades with optional filters
  [[nodiscard]] Result<std::string> get_trades(const TradeFilter &filter = {});

  // ----- Leaderboard -----

  /// Get leaderboard data
  [[nodiscard]] Result<std::string> get_leaderboard();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace polymarket::data
