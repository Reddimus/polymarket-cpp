enable_testing()

# Find or fetch Catch2
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)

# Unit tests
add_executable(polymarket_tests
    unit/crypto_test.cpp
    unit/eip712_test.cpp
    unit/types_test.cpp
    unit/order_builder_test.cpp
    unit/us_client_test.cpp
)

target_link_libraries(polymarket_tests PRIVATE
    polymarket_core
    polymarket_crypto
    polymarket_clob
    polymarket_us
    polymarket_http
    Catch2::Catch2WithMain
)

target_compile_features(polymarket_tests PRIVATE cxx_std_23)

# Register tests with CTest
include(CTest)
include(Catch)
catch_discover_tests(polymarket_tests)
