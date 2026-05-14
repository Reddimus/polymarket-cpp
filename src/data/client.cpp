/// @file data/client.cpp
/// @brief Data API client implementation — STUB (deferred)
///
/// All four methods (`get_positions`, `get_activity`, `get_trades`,
/// `get_leaderboard`) return `Error::network("Data API not yet
/// implemented")`. The Data client targets the offshore Polymarket
/// venue (`data-api.polymarket.com`) which was de-prioritized for
/// PredictionCastAI consumers — production trades exclusively via the
/// CFTC-regulated `polymarket::us::Client`. With CLOB V1's 2026-04-28
/// deprecation, offshore Data work is further deferred pending the V2
/// migration scope decision.
///
/// If/when this is implemented, the pattern mirrors `src/us/client.cpp`:
/// pimpl + cpp-httplib transport + Glaze-shaped response shims +
/// per-endpoint URL builders. See `gamma/client.cpp` for the
/// companion stub.

#include "polymarket/data/client.hpp"

namespace polymarket::data {

struct Client::Impl {
  std::string base_url;
  // HTTP client deferred — see file-level docstring.
};

Client::Client(std::string_view base_url) : impl_(std::make_unique<Impl>()) {
  impl_->base_url = std::string(base_url);
}

Client::~Client() = default;

Client::Client(Client &&) noexcept = default;
Client &Client::operator=(Client &&) noexcept = default;

Result<std::string> Client::get_positions(std::string_view address) {
  return std::unexpected(Error::network("Data API not yet implemented"));
}

Result<std::string> Client::get_activity(std::string_view address) {
  return std::unexpected(Error::network("Data API not yet implemented"));
}

Result<std::string> Client::get_trades(const TradeFilter &filter) {
  return std::unexpected(Error::network("Data API not yet implemented"));
}

Result<std::string> Client::get_leaderboard() {
  return std::unexpected(Error::network("Data API not yet implemented"));
}

} // namespace polymarket::data
