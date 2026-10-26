# polymarket-cpp

Third-party C++23 client with selected Polymarket US REST/WebSocket
methods, signing helpers, and legacy CLOB modules.

## Coverage

- `polymarket::us::Client`: selected public market data and authenticated
  account/order methods. See [declarations](include/polymarket/us/client.hpp).
- `polymarket::us::ws::Subscriber`: market-channel WebSocket subscriber.
- `polymarket::clob`: legacy builders, signing, selected metadata reads, and
  WebSockets. Unimplemented typed book/history/market lists and account,
  cancellation, notification, scoring, and builder operations fail explicitly.
  This release does not implement the current CLOB V2 migration.
- Gamma and Data clients are unsupported: every declared method links but
  returns an explicit error. They do not make HTTP requests.

WebSocket callbacks run on a service thread, may disconnect or destroy the
client, and contain callback exceptions. Incoming messages are limited to
1 MiB and fragments are assembled before delivery. Reconnect explicitly
and register subscriptions again after a close; automatic reconnect
settings in the legacy CLOB configuration are reserved. Use separate CLOB
clients for /ws/market and /ws/user. The credential constructor selects the
user endpoint by default; custom user URLs must end /ws/user. Current user
order/trade events arrive as UserEvent with complete JSON, rather than
being inferred as fills. Other events without a typed decoder, including
price_change arrays, arrive intact as MarketEvent. Unknown tick sizes report errors; heartbeat PONG
frames are ignored. Externally
serialize other operations on each client.

The US and international platforms have separate contracts. Consult the
[US docs](https://docs.polymarket.us/) or
[international docs](https://docs.polymarket.com/) for current service
capabilities; SDK methods do not imply complete platform coverage.

## Build

Use CMake 3.31+, Git, a C++23 compiler (GCC 13+, Clang 19+ with
libstdc++, Clang 18+ with libc++, or current MSVC), OpenSSL 3+, libcurl
7.85+, libwebsockets, and pkg-config on Linux/macOS. Libcurl must support
thread-safe global initialization. Consumer checks also need Python 3.

```sh
# Ubuntu 24.04; install CMake 3.31+ separately if needed.
sudo apt install build-essential git pkg-config clang-format libssl-dev libcurl4-openssl-dev libwebsockets-dev
# macOS: brew install cmake pkg-config clang-format openssl curl libwebsockets
make build
make test
make lint
make consumers
```

CMake fetches secp256k1 0.6.0, private Glaze 9.0.0, and Catch2 3.5.2.
`make debug` uses build-debug. Enable ASan/UBSan on GCC/Clang with
`CMAKE_ARGS=-DPOLYMARKET_ENABLE_SANITIZERS=ON`. Release benchmarks enforce
timing limits; Debug/sanitizer builds check parsing without timing limits.
Tests use fixtures and loopback HTTP/WebSockets; they do not place live orders.

## CMake integration

```cmake
include(FetchContent)
FetchContent_Declare(polymarket_cpp
    GIT_REPOSITORY https://github.com/Reddimus/polymarket-cpp.git
    GIT_TAG v0.5.0)
FetchContent_MakeAvailable(polymarket_cpp)
target_link_libraries(your_target PRIVATE polymarket::polymarket)
```

Installed consumers use `find_package(polymarket CONFIG REQUIRED)` and
`polymarket::polymarket`. Tests, examples, apps, and LTO default to OFF
when embedded. Parent compiler flags are preserved; CPU tuning is opt-in.
OpenSSL, libcurl, and libwebsockets remain system dependencies. The
secp256k1 archive and its license are installed with the SDK.

## First public request

```cpp
#include <polymarket/us/client.hpp>
#include <iostream>

int main() {
    polymarket::us::Client client;
    polymarket::Result<std::string> markets = client.get_markets({.limit = 1});
    if (!markets) {
        std::cerr << markets.error().message() << '\n';
        return 1;
    }
    std::cout << *markets << '\n';
}
```

US authenticated methods require `set_credentials({key_id, secret_key})`,
where secret_key is the platform's base64 secret. See
[authentication](docs/authentication.md) for the CLI command protocol.
Keep secrets outside source control. The `example_us_smoke` program
performs public and account reads; it requires your own credentials.

Legacy CLOB timeout, GET retry, pacing, and server-clock settings are
honored. The timeout is capped at one day; each retry consumes a pacing
token, and writes receive one attempt. Metadata failures propagate rather
than substituting zero fees or a different tick size.

US orders serialize price as a USD amount, quantity as a number, and
time-in-force as tif, matching the [current order schema](https://docs.polymarket.us/api-reference/orders/create-order).
Expiration uses Unix seconds and selects GTD unless time-in-force is
explicit. Nonempty client_order_id values fail because the current REST
schema does not support that field. Order and cancellation acceptance
still requires service validation; default tests use a local server.

## Runtime behavior

HTTP clients initialize libcurl automatically; legacy global_init and
global_cleanup calls remain compatible. Clients require external
synchronization. Requests support HTTP(S), verify TLS certificates, and
limit decompressed bodies to 64 MiB (max_response_bytes=0 disables the
limit). Headers are validated and custom headers disable redirects to
prevent forwarding credentials. Default timeouts are 30 seconds overall
and 10 seconds to connect. Request timeout zero inherits the client
setting. HTTP failures use Result; value parsing and allocation can throw.

`uint256_t` retains all 256 bits in decimal/hex conversions. Hex values
are checked for width and digits; shorter values are left-padded.
Decimal uses signed 64-bit values scaled by 10^6. Parsing truncates extra
fractional digits and rejects malformed/out-of-range input. Arithmetic
throws on overflow; division by zero retains the legacy zero result.
Negative values scale to unsigned zero; positive scaling checks 0..19
places and overflow. See [order building](docs/order-building.md).

See [CONTRIBUTING.md](CONTRIBUTING.md), [CHANGELOG.md](CHANGELOG.md), and
[SECURITY.md](SECURITY.md).
