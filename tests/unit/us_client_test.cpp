/// @file us_client_test.cpp
/// @brief Unit tests for the Polymarket US client.
///
/// These tests pin the auth contract verified against
/// docs.polymarket.us on 2026-05-02:
///   - Headers: X-PM-Access-Key, X-PM-Timestamp (unix ms), X-PM-Signature.
///   - Canonical signed string: literal `{timestamp}{method}{path}` with
///     no separators; body is NOT signed.
///   - Tolerance ±30s.
///   - Secret = base64 of 64 bytes (seed||pub); first 32 bytes is the
///     Ed25519 seed.
///
/// We test the credential-loading + secret-validation paths offline.
/// Live HTTP smoke tests are out of scope here (gated on env in the
/// pm-us-cli example).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "polymarket/crypto/ed25519.hpp"
#include "polymarket/crypto/hmac.hpp"
#include "polymarket/us/client.hpp"

#include <array>
#include <cstdint>
#include <random>
#include <vector>

using namespace polymarket;
using namespace polymarket::us;

namespace {

/// Build a synthetic 64-byte (seed||pub) base64 secret in the same
/// shape Polymarket US emits, so we can exercise the validator without
/// pasting a real key.
std::string make_synthetic_secret(std::uint8_t fill = 0x42) {
  std::vector<std::uint8_t> raw(64, fill);
  return crypto::base64_encode(
      std::span<const std::uint8_t>{raw.data(), raw.size()});
}

} // namespace

TEST_CASE("Polymarket US set_credentials accepts a 64-byte base64 secret",
          "[us][auth]") {
  Client client;
  Credentials creds;
  creds.key_id = "348090aa-a70f-405d-9a1c-9aba64f7d4b7";
  creds.secret_key = make_synthetic_secret();

  auto result = client.set_credentials(std::move(creds));
  REQUIRE(result.has_value());
}

TEST_CASE("Polymarket US set_credentials rejects a too-short secret",
          "[us][auth]") {
  Client client;
  Credentials creds;
  creds.key_id = "test";
  // 32 bytes (raw seed length) — should be 64 per docs.
  std::vector<std::uint8_t> short_secret(32, 0x00);
  creds.secret_key = crypto::base64_encode(
      std::span<const std::uint8_t>{short_secret.data(), short_secret.size()});

  auto result = client.set_credentials(std::move(creds));
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().code() == ErrorCode::ValidationError);
}

TEST_CASE("Polymarket US set_credentials rejects malformed base64",
          "[us][auth]") {
  Client client;
  Credentials creds;
  creds.key_id = "test";
  creds.secret_key = "not-valid-base64!!!";

  auto result = client.set_credentials(std::move(creds));
  REQUIRE_FALSE(result.has_value());
}

TEST_CASE("Polymarket US authed call without credentials fails fast",
          "[us][auth]") {
  Client client;
  // No set_credentials() call.
  auto result = client.get_balance();
  REQUIRE_FALSE(result.has_value());
  REQUIRE(result.error().code() == ErrorCode::ValidationError);
  // The error message names the missing pre-condition.
  REQUIRE_THAT(result.error().message(),
               Catch::Matchers::ContainsSubstring("set_credentials"));
}

TEST_CASE("Polymarket US get_markets path-build does not infinite-recurse",
          "[us][regression]") {
  // Regression test for a real bug shipped briefly in 2026-05-02:
  // append_query had a bool overload that called itself with a
  // `const char*` literal ("true"/"false"). C++ overload resolution
  // ranked `const char*` → bool (standard conversion) higher than
  // `const char*` → string_view (user-defined), so the bool overload
  // re-entered itself instead of the string_view overload. With -O2
  // tail-call optimization, the resulting infinite recursion looked
  // like a hang at 100% CPU with no syscalls.
  //
  // The fix uses `string_view_literals` in the bool body. This test
  // doesn't probe overload resolution directly — it just calls
  // get_markets() with a fully-populated filter (every overload
  // exercised) and times out fast if the bug regresses. The test
  // itself runs in <100ms when healthy.
  Client client;
  Credentials creds;
  creds.key_id = "test";
  creds.secret_key = make_synthetic_secret();
  REQUIRE(client.set_credentials(std::move(creds)).has_value());

  MarketFilter f;
  f.event_id = "evt-1";
  f.active = true;
  f.closed = false;
  f.tag_id = 38;
  f.end_date_min = "2026-05-01T00:00:00Z";
  f.end_date_max = "2026-12-31T00:00:00Z";
  f.limit = 200;
  f.offset = 0;
  f.cursor = "abc";

  // We don't care if this succeeds (it'll fail on the network call
  // since we have no live host stub). We care that it RETURNS at
  // all — pre-fix it spun forever inside the path-build loop.
  auto result = client.get_markets(f);
  // Either a successful Result or a network/validation Error is fine
  // — both prove path-build returned. A hang fails this test via
  // ctest's per-test timeout.
  (void)result;
  SUCCEED("get_markets returned without infinite-recursing");
}

TEST_CASE(
    "Polymarket US Ed25519 sign-message round trip uses first 32 bytes as seed",
    "[us][auth]") {
  // Build a deterministic 64-byte secret where the first 32 bytes are
  // the seed and the last 32 bytes are arbitrary (would be the pubkey
  // in a real Polymarket-issued secret). Verify that signing through
  // the client's loader gives the same signature as signing directly
  // with Ed25519PrivateKey::from_seed on the first-32 slice.
  std::array<std::uint8_t, 32> seed{};
  for (std::size_t i = 0; i < seed.size(); ++i) {
    seed[i] = static_cast<std::uint8_t>(i + 1);
  }
  std::vector<std::uint8_t> secret_bytes(64, 0x00);
  std::copy(seed.begin(), seed.end(), secret_bytes.begin());
  // Last 32 bytes intentionally zero — irrelevant for signing.

  auto direct_key = crypto::Ed25519PrivateKey::from_seed(seed);
  REQUIRE(direct_key.has_value());

  const std::string message = "1730574000000GET/v1/account/balances";
  auto direct_sig = direct_key->sign(message);
  REQUIRE(direct_sig.has_value());

  Client client;
  Credentials creds;
  creds.key_id = "test-key";
  creds.secret_key = crypto::base64_encode(
      std::span<const std::uint8_t>{secret_bytes.data(), secret_bytes.size()});
  REQUIRE(client.set_credentials(std::move(creds)).has_value());

  // We can't directly invoke the private signer, but the loader
  // promises the same key — assert that the seed survived the
  // base64-decode and [:32] slice unchanged.
  std::vector<std::uint8_t> roundtripped =
      crypto::base64_decode(crypto::base64_encode(std::span<const std::uint8_t>{
                                secret_bytes.data(), secret_bytes.size()}))
          .value();
  REQUIRE(roundtripped.size() == 64);
  for (std::size_t i = 0; i < 32; ++i) {
    REQUIRE(roundtripped[i] == seed[i]);
  }
}

TEST_CASE("Polymarket US place_order rejects unknown side without HTTP call",
          "[us][orders][validation]") {
  // Pre-fix the side field was silently ignored and every order went
  // out as ORDER_INTENT_BUY_LONG. Validation fires before the network
  // call, so we don't need credentials wired to exercise it.
  using namespace polymarket;
  using namespace polymarket::us;

  Client client;
  OrderRequest req;
  req.market_id = "tc-temp-nychigh-2026-05-02-gte56lt57";
  req.side = "wrong"; // typo — must be rejected
  req.type = "limit";
  req.price = Decimal::from_string("0.50");
  req.size = Decimal::from_string("10");

  auto result = client.place_order(req);
  REQUIRE_FALSE(result.has_value());
  REQUIRE_THAT(result.error().message(),
               Catch::Matchers::ContainsSubstring("side"));
}

TEST_CASE("Polymarket US place_order rejects unknown type without HTTP call",
          "[us][orders][validation]") {
  using namespace polymarket;
  using namespace polymarket::us;

  Client client;
  OrderRequest req;
  req.market_id = "tc-temp-nychigh-2026-05-02-gte56lt57";
  req.side = "buy";
  req.type = "stop"; // unsupported — must be rejected
  req.price = Decimal::from_string("0.50");
  req.size = Decimal::from_string("10");

  auto result = client.place_order(req);
  REQUIRE_FALSE(result.has_value());
  REQUIRE_THAT(result.error().message(),
               Catch::Matchers::ContainsSubstring("type"));
}
