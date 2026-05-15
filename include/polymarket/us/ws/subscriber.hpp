#pragma once

/// @file ws/subscriber.hpp
/// @brief Minimal libwebsockets-based subscriber for the Polymarket US
///        markets channel (wss://api.polymarket.us/v1/ws/markets).
///
/// Phase-1 scope (matches the Phase-1 plan in
/// `~/.claude/plans/effervescent-strolling-whistle.md`):
///   - One channel per Subscriber: SUBSCRIPTION_TYPE_MARKET_DATA only
///   - Single-shard (Polymarket caps subscriptions at 100 markets each;
///     our USA-only universe is currently ~60 active so one shard fits)
///   - Ed25519-signed handshake via the same canonical message format
///     the REST client uses ({timestamp}{method}{path})
///   - Passive heartbeat handling. The official Polymarket US SDK
///     listens for heartbeat events but does not send unsolicited
///     application heartbeat frames.
///   - Auto-reconnect: caller-driven via a single `connect()` retry on
///     `disconnect`; no exponential ladder yet (Polymarket docs don't
///     specify the right backoff and a fixed 5s retry is the kalshi
///     reference's first-cut value too)
///
/// Deferred (intentional, listed so we don't lose track):
///   - SUBSCRIPTION_TYPE_MARKET_DATA_LITE / TRADE / ORDER channels
///   - Multi-shard >100 markets
///   - Sequence-number tracking + gap detection
///   - Reconnect backoff ladder + jitter
///   - The private socket (api.polymarket.us/v1/ws/private) — gated on
///     authed-trade scope which we don't need until Phase-3 trader

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "polymarket/core/error.hpp"

namespace polymarket::us::ws {

/// Connection-time configuration. The Subscriber uses the auth+host
/// values directly — Credentials must already be resolvable at
/// construction time (we don't refresh mid-stream).
struct SubscriberConfig {
	/// API key id from polymarket.us (a UUID-shaped string).
	std::string key_id;
	/// Base64-encoded 64-byte (seed||pub) Ed25519 secret. The Subscriber
	/// validates length and slices [:32] for the seed, same as the REST
	/// `Client::set_credentials` path.
	std::string secret_key;
	/// Override host for testing. Defaults to api.polymarket.us. Path is
	/// fixed at /v1/ws/markets for this MVP.
	std::string host = "api.polymarket.us";
	/// Retained for ABI/source compatibility with existing services. The
	/// subscriber no longer sends unsolicited application heartbeat frames
	/// because the Polymarket US gateway rejects them as invalid messages.
	std::chrono::seconds heartbeat = std::chrono::seconds{30};
};

/// Callback fired for every inbound text frame. Body is the raw
/// JSON string Polymarket sends — caller is responsible for parsing.
/// Invoked on the libwebsockets service thread, so handlers must be
/// fast (offload to a queue if non-trivial work is needed).
using OnMessage = std::function<void(std::string_view)>;

/// Callback fired when the connection state flips. Useful for the
/// service-side resubscribe-on-reconnect logic.
using OnStateChange = std::function<void(bool connected)>;

class Subscriber {
public:
	Subscriber();
	~Subscriber();

	Subscriber(const Subscriber&) = delete;
	Subscriber& operator=(const Subscriber&) = delete;
	Subscriber(Subscriber&&) noexcept;
	Subscriber& operator=(Subscriber&&) noexcept;

	/// Validate credentials + remember config for the next connect().
	/// Must be called before connect().
	[[nodiscard]] Result<void> configure(SubscriberConfig cfg);

	/// Open the TLS WebSocket and complete the Ed25519 handshake.
	/// Returns when the upgrade succeeds (or with an error if the
	/// upgrade fails / times out at 10 s).
	[[nodiscard]] Result<void> connect();

	/// Close the connection. Idempotent.
	void disconnect();

	[[nodiscard]] bool is_connected() const noexcept;

	/// Send the SUBSCRIPTION_TYPE_MARKET_DATA subscribe message for the
	/// given slugs. The single-shard cap is 100 (per docs); caller is
	/// expected to slice their universe before calling.
	[[nodiscard]] Result<void> subscribe_market_data(const std::vector<std::string>& market_slugs);

	/// Send the SUBSCRIPTION_TYPE_TRADE subscribe message for the given
	/// slugs. Polymarket multiplexes orderbook deltas and executed
	/// trades on the same connection; subscribe to both after connect()
	/// to receive the full market-data feed. Same 100-slug shard cap as
	/// subscribe_market_data().
	[[nodiscard]] Result<void> subscribe_trades(const std::vector<std::string>& market_slugs);

	void on_message(OnMessage cb);
	void on_state_change(OnStateChange cb);

	// Pimpl is exposed publicly because the C-style libwebsockets
	// callback needs to dereference its members. Treat as opaque from
	// user code.
	struct Impl;

private:
	std::unique_ptr<Impl> impl_;
};

} // namespace polymarket::us::ws
