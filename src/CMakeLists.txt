# Core library
add_library(polymarket_core STATIC
    core/error.cpp
    core/types.cpp
)
target_compile_features(polymarket_core PUBLIC cxx_std_23)
target_include_directories(polymarket_core PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# HTTP client library
add_library(polymarket_http STATIC
    http/client.cpp
)
target_link_libraries(polymarket_http PUBLIC polymarket_core CURL::libcurl)
target_include_directories(polymarket_http PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Crypto library
add_library(polymarket_crypto STATIC
    crypto/keccak.cpp
    crypto/secp256k1.cpp
    crypto/eip712.cpp
    crypto/hmac.cpp
    crypto/ed25519.cpp
)
target_link_libraries(polymarket_crypto PUBLIC polymarket_core OpenSSL::SSL OpenSSL::Crypto)
# secp256k1 is referenced only in src/crypto/secp256k1.cpp. Wrapping
# the link in $<LINK_ONLY:...> tells CMake the dependency is purely a
# link-time concern, so it's stripped from INTERFACE_LINK_LIBRARIES on
# install/export. Static archives carry the resolved symbols, so
# downstream consumers neither see nor need the secp256k1 target.
target_link_libraries(polymarket_crypto PUBLIC $<LINK_ONLY:secp256k1>)
target_include_directories(polymarket_crypto PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# CLOB client library
add_library(polymarket_clob STATIC
    clob/client.cpp
    clob/order_builder.cpp
    clob/websocket.cpp
)
if(NOT WIN32)
    set(_POLYMARKET_WS_TARGET PkgConfig::WEBSOCKETS)
else()
    set(_POLYMARKET_WS_TARGET websockets_shared)
endif()

target_link_libraries(polymarket_clob PUBLIC
    polymarket_core
    polymarket_crypto
    polymarket_http
    CURL::libcurl
    ${_POLYMARKET_WS_TARGET}
)
# nlohmann/json is INTERFACE (header-only). Header inclusion path is
# what matters; we expose it PRIVATE-ly because no public polymarket
# header transitively includes it.
target_link_libraries(polymarket_clob PRIVATE nlohmann_json::nlohmann_json)
target_include_directories(polymarket_clob PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Gamma API library
add_library(polymarket_gamma STATIC
    gamma/client.cpp
)
target_link_libraries(polymarket_gamma PUBLIC polymarket_core CURL::libcurl)
target_include_directories(polymarket_gamma PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Data API library
add_library(polymarket_data STATIC
    data/client.cpp
)
target_link_libraries(polymarket_data PUBLIC polymarket_core CURL::libcurl)
target_include_directories(polymarket_data PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Polymarket US library — REST + WS subscriber (markets channel)
add_library(polymarket_us STATIC
    us/client.cpp
    us/ws/subscriber.cpp
)
target_link_libraries(polymarket_us PUBLIC
    polymarket_core
    polymarket_crypto
    polymarket_http
    CURL::libcurl
    ${_POLYMARKET_WS_TARGET}
)
target_include_directories(polymarket_us PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

# Main SDK library (combines all)
add_library(polymarket INTERFACE)
target_link_libraries(polymarket INTERFACE 
    polymarket_core 
    polymarket_crypto 
    polymarket_clob 
    polymarket_gamma 
    polymarket_data 
    polymarket_us
)
