# Changelog

All notable changes to **polymarket-cpp** are recorded here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

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

[Unreleased]: https://github.com/Reddimus/polymarket-cpp/compare/v0.1.1...HEAD
[0.1.1]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.1.1
[0.1.0]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.1.0
