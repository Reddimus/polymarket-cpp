/// @file gamma/client.cpp
/// @brief Gamma API client implementation — STUB (deferred)
///
/// All five methods (`get_events`, `get_event`, `get_markets`,
/// `get_market`, `search`) return `Error::network("Gamma API not yet
/// implemented")`. The Gamma client targets the offshore Polymarket
/// venue (`gamma-api.polymarket.com`) which was de-prioritized for
/// PredictionCastAI consumers — production trades exclusively via the
/// CFTC-regulated `polymarket::us::Client`. With CLOB V1's 2026-04-28
/// deprecation, offshore Gamma work is further deferred pending the V2
/// migration scope decision.
///
/// If/when this is implemented, the pattern mirrors `src/us/client.cpp`:
/// pimpl + cpp-httplib transport + Glaze-shaped response shims +
/// per-endpoint URL builders. Tests would live under `tests/`
/// alongside `test_us_*.cpp`.

#include "polymarket/gamma/client.hpp"

namespace polymarket::gamma {

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

Result<std::string> Client::get_events(const EventFilter &filter) {
  // Deferred — see file-level docstring.
  return std::unexpected(Error::network("Gamma API not yet implemented"));
}

Result<std::string> Client::get_event(std::string_view event_id) {
  return std::unexpected(Error::network("Gamma API not yet implemented"));
}

Result<std::string> Client::get_markets(const MarketFilter &filter) {
  return std::unexpected(Error::network("Gamma API not yet implemented"));
}

Result<std::string> Client::get_market(std::string_view condition_id) {
  return std::unexpected(Error::network("Gamma API not yet implemented"));
}

Result<std::string> Client::search(std::string_view query) {
  return std::unexpected(Error::network("Gamma API not yet implemented"));
}

} // namespace polymarket::gamma
