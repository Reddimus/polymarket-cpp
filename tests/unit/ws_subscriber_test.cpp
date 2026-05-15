/// @file ws_subscriber_test.cpp
/// @brief Unit tests for the Polymarket US WebSocket subscriber.
///
/// These tests pin the lifecycle/move/destruct contract — the
/// subscriber holds an OS thread + libwebsockets context behind a
/// pimpl, so move + destruct must not leak threads or null-deref.
/// Live WS smoke tests against api.polymarket.us are out of scope
/// here (gated on env in the example CLI).

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <utility>
#include <vector>

#include "polymarket/crypto/hmac.hpp"
#include "polymarket/us/ws/subscriber.hpp"

using namespace polymarket;
using namespace polymarket::us::ws;

namespace {

/// Synthetic 64-byte (seed||pub) base64 secret — same shape as the
/// real Polymarket key material, valid enough for configure() but
/// never used to sign live traffic in these tests.
SubscriberConfig make_test_config() {
	std::vector<std::uint8_t> raw(64, 0x42);
	SubscriberConfig cfg;
	cfg.key_id = "test-key-id";
	cfg.secret_key = crypto::base64_encode(std::span<const std::uint8_t>{raw.data(), raw.size()});
	return cfg;
}

} // namespace

TEST_CASE("WS Subscriber configure accepts a valid 64-byte secret", "[us][ws][config]") {
	Subscriber sub;
	auto rc = sub.configure(make_test_config());
	REQUIRE(rc.has_value());
	REQUIRE_FALSE(sub.is_connected());
}

TEST_CASE("WS Subscriber configure rejects too-short secret", "[us][ws][config]") {
	Subscriber sub;
	SubscriberConfig cfg;
	cfg.key_id = "test";
	std::vector<std::uint8_t> short_raw(32, 0x00);
	cfg.secret_key =
		crypto::base64_encode(std::span<const std::uint8_t>{short_raw.data(), short_raw.size()});
	auto rc = sub.configure(std::move(cfg));
	REQUIRE_FALSE(rc.has_value());
}

TEST_CASE("WS Subscriber subscribe before connect rejects with NetworkError", "[us][ws][config]") {
	Subscriber sub;
	REQUIRE(sub.configure(make_test_config()).has_value());
	std::vector<std::string> slugs{"tc-temp-nychigh-2026-05-02-gte56lt57"};
	auto rc = sub.subscribe_market_data(slugs);
	REQUIRE_FALSE(rc.has_value());
}

TEST_CASE("WS Subscriber subscribe with empty slug list is rejected", "[us][ws][validation]") {
	Subscriber sub;
	REQUIRE(sub.configure(make_test_config()).has_value());
	auto rc = sub.subscribe_market_data({});
	REQUIRE_FALSE(rc.has_value());
}

TEST_CASE("WS Subscriber subscribe over the 100-slug shard cap is rejected",
		  "[us][ws][validation]") {
	// Polymarket caps a single SUBSCRIPTION_TYPE_MARKET_DATA frame at
	// 100 slugs. Sharding above that is the caller's responsibility;
	// the SDK should refuse the oversized frame instead of silently
	// truncating.
	Subscriber sub;
	REQUIRE(sub.configure(make_test_config()).has_value());
	std::vector<std::string> slugs;
	slugs.reserve(101);
	for (int i = 0; i < 101; ++i) {
		slugs.push_back("slug-" + std::to_string(i));
	}
	auto rc = sub.subscribe_market_data(slugs);
	REQUIRE_FALSE(rc.has_value());
}

TEST_CASE("WS Subscriber subscribe_trades shares the same validation path",
		  "[us][ws][validation]") {
	// SUBSCRIPTION_TYPE_TRADE goes through the same shard cap and
	// empty-list rejection as SUBSCRIPTION_TYPE_MARKET_DATA.
	Subscriber sub;
	REQUIRE(sub.configure(make_test_config()).has_value());
	// Empty list rejected.
	REQUIRE_FALSE(sub.subscribe_trades({}).has_value());
	// Pre-connect rejected with NetworkError ("not connected").
	std::vector<std::string> slugs{"tc-temp-nychigh-2026-05-02-gte56lt57"};
	auto rc = sub.subscribe_trades(slugs);
	REQUIRE_FALSE(rc.has_value());
	// Oversized list rejected.
	std::vector<std::string> too_many;
	too_many.reserve(101);
	for (int i = 0; i < 101; ++i) {
		too_many.push_back("slug-" + std::to_string(i));
	}
	REQUIRE_FALSE(sub.subscribe_trades(too_many).has_value());
}

TEST_CASE("WS Subscriber move-then-destruct does not crash", "[us][ws][regression][lifecycle]") {
	// Regression test: the defaulted move ctor leaves impl_ nullptr in
	// the moved-from object. The moved-from object's destructor calls
	// disconnect(), which dereferenced impl_ and segfaulted before the
	// null-guard was added in this commit.
	//
	// We exercise both move-construction and move-assignment paths.
	Subscriber a;
	REQUIRE(a.configure(make_test_config()).has_value());

	// Move-construct b from a; both go out of scope here.
	Subscriber b(std::move(a));
	REQUIRE(b.is_connected() == false);
	// a is moved-from — is_connected() must still be safe (returns false).
	REQUIRE(a.is_connected() == false);

	// Move-assign onto a fresh subscriber.
	Subscriber c;
	c = std::move(b);
	REQUIRE(c.is_connected() == false);
	REQUIRE(b.is_connected() == false);
	// Implicit ~Subscriber() on a, b, c follows — must not crash.
	SUCCEED("move + destruct sequence completed without segfault");
}
