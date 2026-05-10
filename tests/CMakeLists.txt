enable_testing()

# Find or fetch Catch2. Force a STATIC build — Catch2's default is
# SHARED on Windows, which kicks in CMake's exports.def auto-generation
# step. That step is known to access-violate on large object lists
# under MSVC + CMake 4 (exit code 0xC0000005); STATIC sidesteps it
# entirely. Linux/macOS keep the same shape.
include(FetchContent)
set(BUILD_SHARED_LIBS_ORIG ${BUILD_SHARED_LIBS})
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
)
FetchContent_MakeAvailable(Catch2)
set(BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS_ORIG} CACHE BOOL "" FORCE)

# Unit tests
add_executable(polymarket_tests
    unit/crypto_test.cpp
    unit/eip712_test.cpp
    unit/types_test.cpp
    unit/order_builder_test.cpp
    unit/us_client_test.cpp
    unit/us_cli_test.cpp
    unit/ws_subscriber_test.cpp
    unit/clob_websocket_test.cpp
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

# us_cli_test exec's the polymarket-us-cli binary via popen; pass its
# resolved path so the test doesn't depend on cwd. Only wired when
# apps/ is being built (the default).
if(POLYMARKET_BUILD_APPS)
    target_compile_definitions(polymarket_tests PRIVATE
        POLYMARKET_US_CLI_PATH="$<TARGET_FILE:polymarket_us_cli>")
    add_dependencies(polymarket_tests polymarket_us_cli)
endif()

# Register tests with CTest
include(CTest)
include(Catch)
catch_discover_tests(polymarket_tests)
