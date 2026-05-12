# polymarket-cpp Development Guide

## Build & Test

```bash
make build              # Release build (CMake + make)
make debug              # Debug build
make test               # Run unit tests (ctest)
make lint               # Check formatting (clang-format --dry-run)
make format             # Format in place
make clean              # Remove build/

make run-market_data        # examples/market_data.cpp (live Polymarket Gamma + CLOB)
make run-order_placement    # examples/order_placement.cpp (needs PRIVATE_KEY env)
```

## Architecture

- **Layered static libraries**: `polymarket_core` → `polymarket_http` → `polymarket_crypto` → `polymarket_clob` / `polymarket_gamma` / `polymarket_data` / `polymarket_us` → `polymarket` (INTERFACE)
- **C++20** (not C++23): cpp-httplib transport, RapidJSON parsing — pragmatic intentional drift from sibling SDKs
- **Crypto**: in-tree implementations of Keccak-256, secp256k1 ECDSA signing, EIP-55 checksum addresses, EIP-712 typed-data hashing, HMAC-SHA256, Ed25519. `secp256k1` ECDSA verify/recover is TODO. macOS test failures (Keccak/secp256k1/EIP-55/EIP-712) are disabled with `if: false` pending OpenSSL 3.0/3.5 brew-vs-apt drift fix.
- **CLOB API**: order builders, EIP-712 signing flow, balance queries. Order/ClobAuth structs hash through EIP-712 domain separators.
- **WebSocket**: functional libwebsockets impl at `src/clob/websocket.cpp` (704 LoC) — covers market + user channels, auto-reconnect, ping interval, the 11-variant `WsMessage` union (book snapshot/delta, price change, last-trade, tick-size change, best-bid-ask, new-market, market-resolved, order fill/cancel, trade confirm). Mirrors the `polymarket::us::ws::Subscriber` lifecycle pattern (pimpl + move-safe + null-guarded destructor). secp256k1 ecrecover via `libsecp256k1` FetchContent (replaces the previous `not implemented` stub).
- **JSON**: [Glaze](https://github.com/stephenberry/glaze) v7.6.0 via FetchContent (compile-time reflection, ~2-4x parse speedup over nlohmann on CLOB orderbook payloads — migrated 2026-05-11). The CLOB WS dispatcher and the `polymarket_us_cli` stdin/stdout envelope both use `glz::generic` because their shapes are dynamic per-message-type / per-command. See `tests/parse_benchmark.cpp` for the regression guard. Gotcha: `glz::generic{object_t{...}}` (variant-converting constructor) serializes via the variant write path (`[index, value]`); build objects with `operator[]=` or `= object_t{}` instead.
- **US client** (`src/us/client.cpp`, shipped in v0.1.0): full Ed25519-signed access to `api.polymarket.us` (authed) + `gateway.polymarket.us` (public). 15+ endpoints — markets list/detail, orderbook, settlement, tags, candles (authed-host routed), account balances, positions, activities, orders. `set_credentials()` validates the base64 secret decodes to exactly 64 bytes and slices `[:32]` for the Ed25519 seed. `http::Client` is `CURLOPT_IPRESOLVE = V4`-pinned to avoid Docker IPv6 reconnect-loops.
- **Stubs**: Gamma/Data clients exist but most endpoints are unimplemented (offshore CLOB scope, deferred).
- **Tests**: Catch2 via FetchContent (24 cases including `[us][regression]` for the v0.1.0 `append_query` bool overload tail-recursion fix).

## Conventions

- Code style: `.clang-format` (LLVM base, tabs, 100 cols)
- Namespace: `polymarket`
- **No `auto`** for local declarations — explicit types preferred. Iterators / structured bindings / lambda closures excepted. (Project doesn't yet ship `cpp_auto_audit.py`; convention is informal but consistent across the codebase.)
- Examples bin names: `example_<topic>` per the Makefile `run-<topic>` target convention shared with sibling SDKs.

## CI

GitHub Actions workflow `.github/workflows/ci.yml`: build on Ubuntu 24.04 + macos-latest, lint via clang-format and markdown-lint. macOS Test step disabled (`if: false`) pending crypto-test root-cause. Release workflow auto-creates a GitHub Release on `vX.Y.Z` tag push (notes extracted from `CHANGELOG.md` via `--notes-file` so inline `code` spans survive). First release `v0.1.0` cut 2026-05-02; bump consumers via FetchContent `GIT_TAG v0.1.x`.
