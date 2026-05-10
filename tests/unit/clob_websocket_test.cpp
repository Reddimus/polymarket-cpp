/// @file clob_websocket_test.cpp
/// @brief Unit tests for the offshore CLOB ``WebSocketClient``.
///
/// These tests pin the validation + lifecycle contract — the client
/// holds an OS thread + libwebsockets context behind a pimpl, so move
/// + destruct must not leak threads or null-deref. Live WS smoke tests
/// against ``wss://ws-subscriptions-clob.polymarket.com`` are out of
/// scope here (no creds, no exchange round-trip on CI). Mirrors the
/// ``polymarket::us::ws::Subscriber`` test shape.

#include <catch2/catch_test_macros.hpp>

#include "polymarket/clob/types.hpp"
#include "polymarket/clob/websocket.hpp"
#include "polymarket/core/types.hpp"

#include <utility>
#include <vector>

using namespace polymarket;
using namespace polymarket::clob;

namespace {

/// Synthetic-but-shape-valid credentials for instantiating the
/// authenticated user-channel constructor. Never used to sign live
/// traffic in these tests.
Credentials make_test_credentials() {
  Credentials c;
  c.api_key = "test-key";
  c.api_secret = "test-secret";
  c.passphrase = "test-pass";
  return c;
}

std::vector<uint256_t> one_asset_id() {
  return std::vector<uint256_t>{uint256_t{}};
}

} // namespace

TEST_CASE("CLOB WebSocketClient default-constructs disconnected and unauth'd",
          "[clob][ws][lifecycle]") {
  WebSocketClient client;
  REQUIRE_FALSE(client.is_connected());
  REQUIRE_FALSE(client.is_authenticated());
  REQUIRE(client.subscription_count() == 0);
}

TEST_CASE("CLOB WebSocketClient with credentials reports authenticated",
          "[clob][ws][lifecycle]") {
  WebSocketClient client(WsConfig{}, make_test_credentials());
  REQUIRE_FALSE(client.is_connected());
  REQUIRE(client.is_authenticated());
}

TEST_CASE("CLOB WebSocketClient default config matches the documented endpoint",
          "[clob][ws][config]") {
  // The base URL hard-coded in WsConfig is the offshore CLOB streaming
  // endpoint. Pinning it here means a typo or accidental rebase onto a
  // staging URL would surface as a test failure rather than silently
  // connecting somewhere wrong in production.
  WebSocketClient client;
  REQUIRE(client.config().url ==
          "wss://ws-subscriptions-clob.polymarket.com/ws/");
  REQUIRE(client.config().auto_reconnect);
  REQUIRE(client.config().max_reconnect_attempts > 0);
  REQUIRE(client.config().ping_interval > std::chrono::seconds{0});
}

TEST_CASE("CLOB subscribe_market before connect rejects with network error",
          "[clob][ws][validation]") {
  WebSocketClient client;
  auto rc = client.subscribe_market(one_asset_id());
  REQUIRE_FALSE(rc.has_value());
  REQUIRE(rc.error().code() == ErrorCode::NetworkError);
}

TEST_CASE("CLOB unsubscribe_market before connect rejects with network error",
          "[clob][ws][validation]") {
  WebSocketClient client;
  auto rc = client.unsubscribe_market(one_asset_id());
  REQUIRE_FALSE(rc.has_value());
  REQUIRE(rc.error().code() == ErrorCode::NetworkError);
}

TEST_CASE("CLOB subscribe_user before connect rejects with network error",
          "[clob][ws][validation]") {
  // Pre-connect rejection takes precedence over the missing-credentials
  // rejection — both branches live on the user path so this also
  // confirms the order of checks is connect-then-creds.
  WebSocketClient client;
  auto rc = client.subscribe_user();
  REQUIRE_FALSE(rc.has_value());
  REQUIRE(rc.error().code() == ErrorCode::NetworkError);
}

TEST_CASE("CLOB SubscriptionId default-invalid", "[clob][ws][types]") {
  // A zero-initialized SubscriptionId must report invalid; the impl
  // allocates ids starting at 1 via fetch_add so 0 is the sentinel
  // for "no allocation yet". Callers depend on this to gate their
  // "do I need to unsubscribe?" logic.
  SubscriptionId sub;
  REQUIRE_FALSE(sub.valid());
  SubscriptionId allocated{42};
  REQUIRE(allocated.valid());
}

TEST_CASE("CLOB WebSocketClient move-then-destruct does not crash",
          "[clob][ws][regression][lifecycle]") {
  // Regression test mirroring ws_subscriber_test.cpp's move test —
  // the defaulted move ctor leaves impl_ nullptr in the moved-from
  // object. A naive destructor that calls disconnect() would
  // deref the nullptr and segfault. The websocket.cpp impl has
  // the equivalent null-guard; this pins it.
  WebSocketClient a;

  // Move-construct b from a.
  WebSocketClient b(std::move(a));
  REQUIRE_FALSE(b.is_connected());
  // a is moved-from — accessors must remain safe.
  REQUIRE_FALSE(a.is_connected());

  // Move-assign onto a fresh client.
  WebSocketClient c;
  c = std::move(b);
  REQUIRE_FALSE(c.is_connected());
  REQUIRE_FALSE(b.is_connected());
  // Implicit ~WebSocketClient on a, b, c follows — must not crash.
  SUCCEED("move + destruct sequence completed without segfault");
}

TEST_CASE("CLOB authenticated client survives move",
          "[clob][ws][regression][lifecycle]") {
  // Same lifecycle test but with credentials in play — exercises the
  // optional<Credentials> move-out path.
  WebSocketClient a(WsConfig{}, make_test_credentials());
  REQUIRE(a.is_authenticated());

  WebSocketClient b(std::move(a));
  REQUIRE(b.is_authenticated());
  // Moved-from object is in a valid-but-unspecified state; calling
  // is_authenticated() must not crash.
  REQUIRE_FALSE(b.is_connected());
  (void)a.is_authenticated();
  (void)a.is_connected();
  SUCCEED("authed move + destruct sequence completed without segfault");
}

TEST_CASE("CLOB callback registration accepts and replaces handlers",
          "[clob][ws][callbacks]") {
  // The on_message / on_error / on_state_change setters take by value;
  // nothing fires until the libwebsockets thread has a payload. Pin
  // that registering does not crash and that re-registering is allowed
  // (so a reconnect loop can swap in a fresh handler without leaking).
  WebSocketClient client;

  int message_count = 0;
  int error_count = 0;
  int state_count = 0;

  client.on_message([&message_count](const WsMessage &) { ++message_count; });
  client.on_error([&error_count](const WsError &) { ++error_count; });
  client.on_state_change([&state_count](bool) { ++state_count; });

  // Replace.
  client.on_message([](const WsMessage &) {});
  client.on_error([](const WsError &) {});
  client.on_state_change([](bool) {});

  // Nothing should have fired without a connected session.
  REQUIRE(message_count == 0);
  REQUIRE(error_count == 0);
  REQUIRE(state_count == 0);
}
