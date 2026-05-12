# Changelog

All notable changes to **polymarket-cpp** are recorded here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.4.0] - 2026-05-12

### Changed

- **JSON library**: migrated from nlohmann/json v3.11.3 to
  [Glaze](https://github.com/stephenberry/glaze) v7.6.0 via FetchContent
  (commit on `feat/glaze-migration`, 2026-05-11). Glaze's compile-time
  reflection delivers a ~2-4x parse speedup on representative CLOB
  orderbook payloads (200 levels/side, ~14.6KB) — measured median on
  x86_64-v3 GCC 13.3 -O3 -DNDEBUG: nlohmann ~500 us/op → glaze
  ~290 us/op (Decimal::from_string dominates the per-level work; the
  JSON parse itself is ≈3x faster but fixed-cost relative to the 400
  decimal conversions per parse).
- Affected code: `src/clob/websocket.cpp` (the CLOB WS inbound
  variant-dispatch parser) and `apps/polymarket_us_cli.cpp` (the
  stdin/stdout JSON envelope). Both use `glz::generic` because their
  shapes are dynamic per-message-type / per-command — a static
  `glz::meta` per type would be heavier on maintenance with no
  measurable speedup, given how few REST response models exist in the
  SDK at this point (CLOB and Gamma client structs are stubs; the
  US REST client returns `Result<std::string>` and lets callers parse).
- Public SDK API (`polymarket::clob::*`, `polymarket::us::*`,
  `polymarket::crypto::*`) is unchanged. Consumers (polymarket-trader,
  polymarket-data) build without modification.
- New tests: `tests/unit/glaze_test.cpp` (shape-parity guard with 6
  Catch2 cases including a bool/string overload-set regression) and
  `tests/parse_benchmark.cpp` (1k-iteration parse benchmark, capped at
  1000 us/op via `ctest --timeout`). CMakeLists install-list no longer
  re-exports `nlohmann_json`. Glaze is exposed as a build-interface
  include path (matching how nlohmann was integrated pre-migration)
  to sidestep CMake's "INTERFACE target not in export set" error on
  the installable static libs.

## [0.3.1] - 2026-05-10

### Fixed

- Renamed `FetchContent_Declare(nlohmann_json ...)` → `FetchContent_Declare(json ...)`
  to match the convention every sibling consumer (`infra-cpp/kalshi`,
  `open-meteo-data`, `nws-data`, `kalshi-market-data`,
  `polymarket-trader` transitively via infra-cpp) already uses. v0.3.0
  introduced this dep with the `nlohmann_json` name and that broke
  `polymarket-trader`'s build with
  `add_library cannot create target "nlohmann_json"` because two
  different FetchContent names ("json" + "nlohmann_json") both tried
  to create the same target. With matching names, FetchContent's
  first-registered-wins dedupe kicks in and a single `nlohmann_json`
  target is shared across the dep tree.

## [0.3.0] - 2026-05-10

### Added

- **`pm::clob::WebSocketClient`** — functional libwebsockets implementation
  for the offshore CLOB streaming API. Replaces the previous Phase-10 stub.
  Covers both the public market channel (orderbook snapshot/delta, price
  change, last-trade-price, tick-size change, plus the optional custom
  features `best_bid_ask`, `new_market`, `market_resolved`) and the
  authenticated user channel (`order_fill`, `order_cancel`,
  `trade_confirm`). Uses the same pimpl + move-safe lifecycle pattern as
  `pm::us::ws::Subscriber`. Auto-reconnect, configurable ping interval,
  and a typed `WsMessage` variant for callback dispatch.
- **`pm::crypto::secp256k1::recover_pubkey_from_signature`** — full ECDSA
  public-key recovery via Bitcoin Core's libsecp256k1 (vendored through
  `FetchContent` against the upstream `v0.6.0` tag). Replaces the
  `Error::crypto("not implemented")` stub. OpenSSL has no recovery
  primitive, which is why this needed a separate dep. Round-trip
  unit test pins sign → recover → pubkey-equality.
- Cross-platform CI: Windows `build-windows` job added on every SDK in
  the family, and macOS Test step re-enabled now that the Apple Clang
  Keccak `rotl64` UB on `n=0` (root cause of the long-standing macOS
  divergence) is fixed.

### Fixed

- **Apple Clang Keccak digest divergence on macOS** — hand-rolled
  `rotl64` was `(x << n) | (x >> (64 - n))`, which is undefined behavior
  when `n == 0` (Keccak's `ROTATION[0][0] == 0` hits this once per
  round). GCC and Linux Clang happened to fold the UB to `0`; Apple
  Clang at `-O2` exploited the UB and produced silently wrong digests,
  breaking Keccak-256 / EIP-55 / EIP-712 / Address tests. Switched to
  C++20 `std::rotl`, which is well-defined on every compiler.
- Windows portability: `Method::DELETE` collided with the
  `<windows.h>` `DELETE` macro; renamed to `Method::DEL`.
  Replaced `__uint128_t` with a portable `mul_div_u64` helper that
  uses `_umul128` / `_udiv128` intrinsics on MSVC. `gmtime_r` →
  `gmtime_s` branch on `_WIN32`. `popen`/`pclose` aliased to
  `_popen`/`_pclose` for the `us_cli_test`. Catch2 forced to STATIC
  to dodge MSVC SHARED-build DLL-export crashes.

## [0.2.0] - 2026-05-04

### Added

- **`pm::us::Client::get_order_by_id(order_id)`** — fetch a single
  order's status by id (`GET /v1/order/{id}`). Used by trader-side
  reconciliation to promote `live_pending` → `live_filled` without
  scanning the full open-orders list.
- **`apps/polymarket-us-cli`** — new production binary that wraps
  `pm::us::Client` behind a JSON-stdin/JSON-stdout protocol. One
  invocation = one operation. Lets Python services like
  polymarket-trader's LiveExecutor talk to Polymarket US without
  re-implementing Ed25519 signing or the wire format. Commands:
  `get_account` | `get_balance` | `place_order` | `cancel_order` |
  `get_order` | `get_orders` | `get_positions`. Structured error
  envelope with categories (`auth`, `bad_request`, `transport`,
  `unknown`) so callers can decide retry-vs-not without parsing
  free-text.
- Tests: 9 new `us_cli` integration tests via popen — version
  output, argument parsing, credential loading (JSON + colon
  formats), stdin protocol enforcement, command dispatch validation
  for each parameterized command.

### Build

- New `apps/` subdirectory + `POLYMARKET_BUILD_APPS` CMake option
  (default ON). The CLI links against `nlohmann/json` v3.11.3 via
  FetchContent; this dependency is scoped to apps/ only — the
  library's public headers stay JSON-lib-agnostic.

## [0.1.5] - 2026-05-03

### Added

- **`polymarket::us::ws::Subscriber::subscribe_trades(slugs)`** — sends
  `SUBSCRIPTION_TYPE_TRADE` for the given market slugs. Polymarket
  multiplexes orderbook deltas and executed trades on the same
  connection, but each subscription type is its own subscribe frame.
  Without this, the consumer had no way to ask for trades; the
  `polymarket-tick-archiver`'s newly-shipped trade-stream archiver
  was sitting at `trades_ok=0` indefinitely. Same 100-slug shard cap
  + same validation path as `subscribe_market_data`.
- Internal: `build_subscribe_frame` now takes the subscription-type
  literal as a parameter, and slug validation is shared between the
  two subscribe methods via a small `validate_slugs` helper.
- Test: `subscribe_trades` shares the same empty-list /
  not-connected / 100-slug-cap validation as `subscribe_market_data`
  (1 new case, suite total now 33 was 32).

## [0.1.4] - 2026-05-02

### Fixed

- **`polymarket::us::Client::place_order()` now honors `OrderRequest.side`
  and `OrderRequest.type` instead of hardcoding `ORDER_INTENT_BUY_LONG`
  / `ORDER_TYPE_LIMIT`.** A trader passing `side="sell"` previously
  submitted a BUY order silently; any typo or unsupported value did
  the same. The mapping is `buy → BUY_LONG`, `sell → SELL_LONG`,
  `limit → LIMIT`, `market → MARKET`, with `post_only=true` emitted
  as `"participateDontInitiate":true` per docs. Unknown values now
  return `Error::validation` before any HTTP call. Short-side
  intents (`BUY_SHORT` / `SELL_SHORT`) are deliberately not mapped
  from a string — they warrant their own typed entry point. Two
  regression tests added (26 total, was 24).

## [0.1.3] - 2026-05-02

### Fixed

- **`polymarket::us::ws::Subscriber::connect()` is now idempotent.**
  Calling `connect()` after a transient disconnect (e.g. an upstream
  502) used to move-assign a fresh `std::thread` over the old
  service thread while it was still joinable, which calls
  `std::terminate()` and crashes the host process. The connect path
  now tears down the prior context + thread via `disconnect()`
  before standing up the new ones. Observed in the
  `polymarket-websocket` service as a single
  `terminate called without an active exception` after a 502.

## [0.1.2] - 2026-05-02

### Added

- **`polymarket::us::ws::Subscriber`** — first-cut libwebsockets-based
  WebSocket subscriber for `wss://api.polymarket.us/v1/ws/markets`.
  Performs the Ed25519-signed handshake (same canonical message format
  as the REST client: `{timestamp}{method}{path}`), sends the
  `SUBSCRIPTION_TYPE_MARKET_DATA` subscribe frame, drives a heartbeat
  on a configurable interval, and surfaces inbound frames via an
  `OnMessage` callback. Single-shard (≤100 markets per Polymarket's
  documented cap), single-channel, no exponential reconnect ladder
  yet — caller drives reconnect via a simple `connect()` retry on
  state-change. See header comment for the deferred-feature list.
- New `examples/us_smoke.cpp` is unaffected; the new subscriber lives
  alongside the REST client in `polymarket_us` so consumers don't
  need a new FetchContent target.

[0.1.2]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.1.2

## [0.1.1] - 2026-05-02

### Added

- **`examples/us_smoke.cpp`** — operator-runnable end-to-end smoke
  test for the US client. Reads `PM_US_KEY_ID` + `PM_US_SECRET_FILE`
  from env, validates Ed25519 setup, and hits 5 endpoints
  (`/v1/health`, `/v2/tags/slug/weather`, `/v1/markets`,
  `/v1/account/balances`, `/v1/portfolio/positions`) in <1s.
  Wired through `make run-us_smoke`.
- **README**: new "Polymarket US (Ed25519, CFTC-regulated)" usage
  section + API Coverage table updated to reflect v0.1.0 surface
  (was still marked ⏳ Stub).

### Fixed

- **`.github/workflows/release.yml`** preserves backticks in the
  release body. The previous version captured the changelog
  excerpt into a shell variable then `echo`-ed it into
  `GITHUB_OUTPUT`; bash treated backticks as command substitution
  and silently stripped every inline `code` span. Now uses
  `--notes-file` for byte-preserving release notes.

### Updated

- **`CLAUDE.md`** refreshed post-v0.1.0: US client now documented
  as implemented, tests are Catch2 (not GoogleTest), v0.1.0
  tagged + first release notes.

## [0.1.0] - 2026-05-02

### Added

- **Polymarket US REST client** (`polymarket::us::Client`) — full
  Ed25519-signed access to `api.polymarket.us` (authed) and
  `gateway.polymarket.us` (public). 15+ endpoints: markets list +
  detail, orderbook, settlement, tags, candles, account balances,
  positions, activities, orders. `set_credentials()` validates the
  base64 secret decodes to exactly 64 bytes and slices `[:32]` for
  the Ed25519 seed per the docs.polymarket.us spec.
- **Canonical signing**: literal `{timestamp}{method}{path}` (no
  separators, body NOT signed); ±30s tolerance window respected via
  `now_unix_ms()`. Path component is signed without query string.
- **Gateway endpoints**: `get_tag_by_slug`, `get_settlement`,
  `get_candles`, `get_health` against the appropriate host.

### Fixed

- **`append_query` bool overload was tail-recursing under -O2**:
  the `const char*` literal `"true"` / `"false"` bound to the bool
  overload via standard pointer-to-bool conversion (which ranks
  higher than the user-defined string_view conversion). Symptom was
  100% CPU spin with no syscalls inside `get_markets()`. Fix uses
  `std::string_view_literals`. Pinned by a `[us][regression]`
  Catch2 test that hangs forever pre-fix and returns in <1ms post-fix.
- **`http::Client` forced to IPv4** via `CURLOPT_IPRESOLVE = V4`.
  Docker bridge networks resolve AAAA records but route only IPv4;
  without this, libcurl tried IPv6 first and either blocked for the
  full 30s timeout or, on certain libcurl/glibc combos, spun in a
  tight reconnect loop.
- **`get_candles` routes through the authed host**, not the public
  gateway. Field names normalized to camelCase
  (`startTimeMs`/`endTimeMs`) per the docs response shape.
- **CMake**: `polymarket_us` now correctly links `polymarket_http`
  (previously relied on transitive curl from a sibling target;
  downstream FetchContent consumers got undefined-reference errors).
- **CMake**: `${CMAKE_SOURCE_DIR}` → `${PROJECT_SOURCE_DIR}` in
  `src/CMakeLists.txt` so includes resolve correctly when the SDK
  is consumed via FetchContent rather than as the top-level project.

### CI

- First-ever CI workflow added — build + test + lint on Ubuntu 24.04,
  build-only on macos-latest (test step disabled pending investigation
  of crypto-test failures).
- `.markdownlint-cli2.yaml` config disables `MD013` and other style-
  noise rules; enforces blank-line discipline.
- `.github/workflows/release.yml` auto-creates a GitHub Release when
  a `vX.Y.Z` tag is pushed (body sourced from this CHANGELOG).

### Known issues

- macOS `Test` step is disabled (`if: false`) pending root-cause of:
  - Test #1 Keccak-256 test vectors (hash output differs from Linux)
  - Test #3 EIP-55 checksum addresses
  - Test #4 secp256k1 signing
  - Test #7 EIP-712 Domain Separator

  Likely upstream OpenSSL version drift between brew (HEAD LLVM
  + OpenSSL 3.5+) and apt (OpenSSL 3.0). The Build step still verifies
  macOS portability of the compile path.

### Initial scope

This project has not yet cut a versioned release. Current capabilities:

- **CLOB API**: order builders, EIP-712 signing flow, balance queries,
  ClobAuth + Order struct hashing, types layer (`uint256_t`, `Address`,
  `Decimal`, `Bytes32`)
- **Crypto**: Keccak-256, secp256k1, EIP-712, HMAC-SHA256, Ed25519
- **HTTP**: libcurl wrapper with retry + timeout configuration

Stubs (TODO: implement):

- Gamma API client
- Data API client
- US (US-restricted markets) client
- WebSocket subscriptions / order-history streaming
- ECDSA public-key recovery in secp256k1 (signing works; recovery TODO)

[Unreleased]: https://github.com/Reddimus/polymarket-cpp/compare/v0.4.0...HEAD
[0.4.0]: https://github.com/Reddimus/polymarket-cpp/compare/v0.3.1...v0.4.0
[0.3.1]: https://github.com/Reddimus/polymarket-cpp/compare/v0.3.0...v0.3.1
[0.3.0]: https://github.com/Reddimus/polymarket-cpp/compare/v0.2.0...v0.3.0
[0.2.0]: https://github.com/Reddimus/polymarket-cpp/compare/v0.1.5...v0.2.0
[0.1.5]: https://github.com/Reddimus/polymarket-cpp/compare/v0.1.4...v0.1.5
[0.1.4]: https://github.com/Reddimus/polymarket-cpp/compare/v0.1.3...v0.1.4
[0.1.3]: https://github.com/Reddimus/polymarket-cpp/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.1.2
[0.1.1]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.1.1
[0.1.0]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.1.0
