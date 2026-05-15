/// @file gamma/client.hpp
/// @brief Gamma API client for market metadata

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "polymarket/core/error.hpp"

namespace polymarket::gamma {

/// Filter for listing events
struct EventFilter {
	std::optional<std::string> slug;
	std::optional<bool> active;
	std::optional<bool> closed;
	std::optional<std::string> tag;
	std::optional<int> limit;
	std::optional<std::string> cursor;
};

/// Filter for listing markets
struct MarketFilter {
	std::optional<std::string> event_id;
	std::optional<bool> active;
	std::optional<int> limit;
	std::optional<std::string> cursor;
};

/// Gamma API client for market metadata
///
/// The Gamma API provides:
/// - Event listings and details
/// - Market definitions and resolution info
/// - Token ID mappings
/// - Series and tag information
///
/// Base URL: https://gamma-api.polymarket.com
class Client {
public:
	/// Create a new Gamma API client
	/// @param base_url Base URL (default: https://gamma-api.polymarket.com)
	explicit Client(std::string_view base_url = "https://gamma-api.polymarket.com");
	~Client();

	Client(const Client&) = delete;
	Client& operator=(const Client&) = delete;
	Client(Client&&) noexcept;
	Client& operator=(Client&&) noexcept;

	// ----- Events -----

	/// List events with optional filters
	[[nodiscard]] Result<std::string> get_events(const EventFilter& filter = {});

	/// Get event by ID
	[[nodiscard]] Result<std::string> get_event(std::string_view event_id);

	/// Get event by slug
	[[nodiscard]] Result<std::string> get_event_by_slug(std::string_view slug);

	// ----- Markets -----

	/// List markets with optional filters
	[[nodiscard]] Result<std::string> get_markets(const MarketFilter& filter = {});

	/// Get market by condition ID
	[[nodiscard]] Result<std::string> get_market(std::string_view condition_id);

	// ----- Series -----

	/// List all series
	[[nodiscard]] Result<std::string> get_series();

	/// Get series by ticker
	[[nodiscard]] Result<std::string> get_series_by_ticker(std::string_view ticker);

	// ----- Tags -----

	/// List all tags
	[[nodiscard]] Result<std::string> get_tags();

	// ----- Sports -----

	/// List all sports
	[[nodiscard]] Result<std::string> get_sports();

	// ----- Search -----

	/// Search events and markets
	[[nodiscard]] Result<std::string> search(std::string_view query);

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace polymarket::gamma
