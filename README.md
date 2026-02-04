# polymarket-cpp

A production-grade C++ SDK for [Polymarket](https://polymarket.com) prediction market APIs.

[![Build](https://github.com/yourorg/polymarket-cpp/actions/workflows/build.yml/badge.svg)](https://github.com/yourorg/polymarket-cpp/actions)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)

## Features

- **Full CLOB API Coverage**: Orders, markets, prices, order books, and more
- **Type-Safe Order Builders**: Compile-time validation for limit and market orders
- **EIP-712 Signing**: Complete cryptographic support for Ethereum-compatible signatures
- **Modern C++23**: Uses `std::expected`, `std::span`, and other modern features
- **CMake Integration**: Easy to integrate via FetchContent or find_package
- **Comprehensive Testing**: Unit tests for all crypto and core functionality

## Architecture

```mermaid
graph TB
    subgraph "polymarket-cpp"
        subgraph "API Clients"
            CLOB[CLOB Client<br/>Orders, Markets, Prices]
            Gamma[Gamma Client<br/>Events, Markets, Series]
            Data[Data Client<br/>Positions, Trades]
            US[US Client<br/>Ed25519 Auth]
        end
        
        subgraph "Core"
            HTTP[HTTP Client<br/>libcurl wrapper]
            WS[WebSocket<br/>libwebsockets]
            Types[Types<br/>uint256, Address, Decimal]
            Error[Error Handling<br/>std::expected]
        end
        
        subgraph "Crypto"
            Keccak[Keccak-256]
            ECDSA[secp256k1<br/>ECDSA signing]
            EIP712[EIP-712<br/>Typed data hashing]
            HMAC[HMAC-SHA256]
            Ed25519[Ed25519]
        end
    end
    
    CLOB --> HTTP
    CLOB --> EIP712
    CLOB --> ECDSA
    Gamma --> HTTP
    Data --> HTTP
    US --> HTTP
    US --> Ed25519
    
    EIP712 --> Keccak
    ECDSA --> Keccak
```

## Quick Start

### Prerequisites

- C++23 compatible compiler (GCC 13+, Clang 17+, MSVC 2022)
- CMake 3.20+
- OpenSSL 3.x
- libcurl

### Building

```bash
# Clone the repository
git clone https://github.com/yourorg/polymarket-cpp.git
cd polymarket-cpp

# Build
make build

# Run tests
make test
```

### Using with CMake FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(
    polymarket-cpp
    GIT_REPOSITORY https://github.com/yourorg/polymarket-cpp.git
    GIT_TAG main
)
FetchContent_MakeAvailable(polymarket-cpp)

add_executable(my_bot main.cpp)
target_link_libraries(my_bot PRIVATE polymarket::clob)
```

### Using with find_package

```cmake
find_package(polymarket-cpp REQUIRED)
target_link_libraries(my_bot PRIVATE polymarket::clob)
```

## Usage Examples

### Fetch Market Data (Unauthenticated)

```cpp
#include <polymarket/clob/client.hpp>
#include <iostream>

int main() {
    using namespace polymarket::clob;
    
    // Create unauthenticated client
    auto client = Client::create("https://clob.polymarket.com");
    
    // Get server time
    auto time_result = client.get_server_time();
    if (time_result) {
        std::cout << "Server time: " << *time_result << std::endl;
    }
    
    // Get markets
    MarketsRequest req;
    req.next_cursor = std::nullopt;
    
    auto markets = client.get_markets(req);
    if (markets) {
        for (const auto& market : markets->data) {
            std::cout << "Market: " << market.question << std::endl;
        }
    }
    
    return 0;
}
```

### Place Orders (Authenticated)

```cpp
#include <polymarket/clob/client.hpp>
#include <polymarket/crypto/secp256k1.hpp>

int main() {
    using namespace polymarket;
    using namespace polymarket::clob;
    using namespace polymarket::crypto;
    
    // Load private key
    auto key = PrivateKey::from_hex("0x...");
    if (!key) {
        std::cerr << "Failed to load key" << std::endl;
        return 1;
    }
    
    auto address = key->address();
    if (!address) {
        return 1;
    }
    
    // Create authenticated client
    ClientConfig config;
    config.base_url = "https://clob.polymarket.com";
    config.chain_id = 137;  // Polygon mainnet
    
    auto client = Client::create(config);
    client.set_signer(*address);
    
    // Build and sign a limit order
    auto builder = client.limit_order_builder();
    builder.token_id(uint256_t::from_string("12345"));
    builder.side(Side::Buy);
    builder.price(Decimal("0.50"));
    builder.size(Decimal("100"));
    
    auto order = builder.build();
    if (!order) {
        std::cerr << "Order validation failed: " << order.error().message() << std::endl;
        return 1;
    }
    
    // Sign the order
    auto signed_order = client.sign_order(*order, *key);
    if (!signed_order) {
        return 1;
    }
    
    // Place the order
    auto result = client.post_order(*signed_order);
    if (result) {
        std::cout << "Order placed: " << result->id << std::endl;
    }
    
    return 0;
}
```

## Authentication Flow

```mermaid
sequenceDiagram
    participant User
    participant Client
    participant CLOB API
    participant Blockchain
    
    Note over User,Blockchain: L1 Authentication (Derive API Key)
    User->>Client: Provide Private Key
    Client->>Client: Create ClobAuth struct
    Client->>Client: EIP-712 sign ClobAuth
    Client->>CLOB API: POST /auth/derive-api-key<br/>Headers: POLY_ADDRESS, POLY_SIGNATURE, POLY_TIMESTAMP, POLY_NONCE
    CLOB API-->>Client: API Key + Secret
    
    Note over User,Blockchain: L2 Authentication (Trading)
    User->>Client: Place Order
    Client->>Client: HMAC-SHA256 sign request
    Client->>CLOB API: POST /order<br/>Headers: POLY_API_KEY, POLY_PASSPHRASE, POLY_SIGNATURE
    CLOB API->>Blockchain: Submit matched trade
    Blockchain-->>CLOB API: Confirmation
    CLOB API-->>Client: Order Response
    Client-->>User: Order ID
```

## Order Builder Flow

```mermaid
flowchart TD
    Start([Start]) --> CreateBuilder[Create OrderBuilder]
    CreateBuilder --> SetToken[Set token_id]
    SetToken --> SetSide[Set side: Buy/Sell]
    SetSide --> IsLimit{Limit Order?}
    
    IsLimit -->|Yes| SetPrice[Set price]
    SetPrice --> SetSize[Set size in shares]
    SetSize --> Validate
    
    IsLimit -->|No| SetAmount[Set amount<br/>USDC or shares]
    SetAmount --> Validate
    
    Validate{Validate} -->|Price out of range| Error1[ValidationError:<br/>Price must be 0.001-0.999]
    Validate -->|Missing field| Error2[ValidationError:<br/>Required field missing]
    Validate -->|Valid| Build[Build SignableOrder]
    
    Build --> Sign[Sign with PrivateKey]
    Sign --> Submit[Submit to CLOB API]
    
    Error1 --> End([End])
    Error2 --> End
    Submit --> End
```

## API Coverage

| API | Endpoint | Status |
|-----|----------|--------|
| **CLOB** | Health, Time | ✅ |
| | Markets | ✅ |
| | Order Book | ✅ |
| | Prices | ✅ |
| | Orders (CRUD) | ✅ |
| | Trades | ✅ |
| | Balance/Allowance | ✅ |
| | API Key Management | ✅ |
| **Gamma** | Events | ⏳ Stub |
| | Markets | ⏳ Stub |
| | Series | ⏳ Stub |
| **Data** | Positions | ⏳ Stub |
| | Trades | ⏳ Stub |
| **US** | Events/Markets | ⏳ Stub |
| | Orders | ⏳ Stub |

## Directory Structure

```
polymarket-cpp/
├── CMakeLists.txt          # Root build configuration
├── Makefile                # Convenience targets
├── include/
│   └── polymarket/
│       ├── polymarket.hpp  # Umbrella header
│       ├── core/           # Error, types, utilities
│       ├── crypto/         # Keccak, secp256k1, EIP-712
│       ├── clob/           # CLOB API client
│       ├── gamma/          # Gamma API client
│       ├── data/           # Data API client
│       └── us/             # Polymarket US client
├── src/                    # Implementation files
├── tests/                  # Unit and integration tests
├── examples/               # Usage examples
└── docs/                   # Additional documentation
```

## Configuration Options

| Option | Default | Description |
|--------|---------|-------------|
| `POLYMARKET_BUILD_TESTS` | ON | Build unit tests |
| `POLYMARKET_BUILD_EXAMPLES` | ON | Build examples |
| `POLYMARKET_USE_CPP26` | OFF | Enable C++26 features (experimental) |

## Error Handling

The library uses `std::expected<T, Error>` for error handling:

```cpp
auto result = client.get_order_book(token_id);
if (result) {
    // Success: use *result
    for (const auto& bid : result->bids) {
        std::cout << bid.price << " x " << bid.size << std::endl;
    }
} else {
    // Error: check result.error()
    switch (result.error().code()) {
        case ErrorCode::NetworkError:
            std::cerr << "Network error: " << result.error().message() << std::endl;
            break;
        case ErrorCode::AuthError:
            std::cerr << "Authentication failed" << std::endl;
            break;
        default:
            std::cerr << "Error: " << result.error().message() << std::endl;
    }
}
```

## Contributing

Contributions are welcome! Please see [CONTRIBUTING.md](docs/CONTRIBUTING.md) for guidelines.

## License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## Disclaimer

This is an unofficial, third-party SDK. It is not affiliated with, endorsed by, or sponsored by Polymarket. Use at your own risk. Trading on prediction markets involves financial risk.

## See Also

- [Polymarket Documentation](https://docs.polymarket.com)
- [Official Python SDK](https://github.com/Polymarket/py-clob-client)
- [Official TypeScript SDK](https://github.com/Polymarket/clob-client)
- [kalshi-cpp](https://github.com/yourorg/kalshi-cpp) - Similar C++ SDK for Kalshi
