# Changelog

All notable changes to **polymarket-cpp** are recorded here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.5.0] - 2026-10-25

### Fixed

- Preserve full-width uint256 decimal values; reject oversized/invalid hex.
- Check decimal parsing, arithmetic, scaling, and floating conversions;
  preserve negative fractional signs. Invalid values and overflow now throw.
- Join/retire WebSocket workers safely across moves, callback destruction,
  and reconnects; assemble bounded messages and preserve TLS runtime state.
- Bound HTTP responses and callbacks, validate headers, restrict protocols,
  inherit configured timeouts, and protect credentials across redirects.
- Encode US query values, validate complete JSON and integer conversions,
  and contain exceptions at the CLOB WebSocket callback boundary.
- Define all declared Gamma methods and replace fabricated CLOB results
  with explicit unsupported errors. Validate metadata and order acceptance.
- Match current US order fields (amount price, numeric quantity, tif, manual
  indicator, and GTD expiration); escape bodies and reject unsupported IDs.
- Separate CLOB market/user sockets, match authenticated subscription JSON,
  retain raw user events/trade sides, and reject unknown WebSocket tick sizes.
- Correct installed dependencies, private include paths, version reporting,
  incremental/Debug builds, strict lint, hooks, and concise documentation.

### Changed

- Require CMake 3.31, OpenSSL 3, libcurl 7.85, and private Glaze 9.
- Disable tests/examples/apps/LTO by default in embedded builds and retain
  parent compiler flags. Add sanitizer and consumer integration checks.

## [0.4.3] - 2026-05-19

- Add shared C++ formatting/editor settings and clarify SDK coverage.
- Complete the license holder name.

## [0.4.2] - 2026-05-14

- Deprecate legacy CLOB V1 trading after the upstream V2 cutover.

## [0.4.1] - 2026-05-12

- Remove unsupported US WebSocket application heartbeat frames.

## [0.4.0] - 2026-05-12

- Replace nlohmann/json with private Glaze parsing in the CLI and CLOB WebSocket.
- Add JSON shape regressions and a parse benchmark.

## [0.3.1] - 2026-05-10

- Align the JSON FetchContent name with downstream consumers.

## [0.3.0] - 2026-05-10

- Implement CLOB WebSocket channels and secp256k1 public-key recovery.
- Fix undefined rotation in Keccak and Windows portability issues.
- Enable macOS tests and add Windows CI. Automatic CLOB reconnect remained
  incomplete; 0.5.0 documents explicit reconnects.

## [0.2.0] - 2026-05-04

- Add single-order lookup and the stdin/stdout JSON CLI protocol.

## [0.1.5] - 2026-05-03

- Add US trade subscriptions with shared slug validation.

## [0.1.4] - 2026-05-02

- Honor US order side/type instead of silently substituting buy/limit.

## [0.1.3] - 2026-05-02

- Clean up the previous US WebSocket worker before reconnecting.

## [0.1.2] - 2026-05-02

- Add the US market WebSocket subscriber and Ed25519 handshake.

## [0.1.1] - 2026-05-02

- Add a manual US account smoke test and preserve release-note literals.

## [0.1.0] - 2026-05-02

- Add selected US REST methods and Ed25519 authentication.
- Fix query overload recursion and transitive HTTP linking.
- Include legacy CLOB builders, crypto, and HTTP modules; Gamma/Data remained stubs.

[Unreleased]: https://github.com/Reddimus/polymarket-cpp/compare/v0.5.0...HEAD
[0.5.0]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.5.0
[0.4.3]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.4.3
[0.4.2]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.4.2
[0.4.1]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.4.1
[0.4.0]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.4.0
[0.3.1]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.3.1
[0.3.0]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.3.0
[0.2.0]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.2.0
[0.1.5]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.1.5
[0.1.4]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.1.4
[0.1.3]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.1.3
[0.1.2]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.1.2
[0.1.1]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.1.1
[0.1.0]: https://github.com/Reddimus/polymarket-cpp/releases/tag/v0.1.0
