enable_testing()

include(FetchContent)
set(CATCH_INSTALL_DOCS OFF CACHE BOOL "" FORCE)
set(CATCH_INSTALL_EXTRAS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.5.2
    EXCLUDE_FROM_ALL)
block()
    set(BUILD_SHARED_LIBS OFF)
    FetchContent_MakeAvailable(Catch2)
endblock()
FetchContent_GetProperties(Catch2)
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
set_target_properties(Catch2 Catch2WithMain PROPERTIES EXCLUDE_FROM_ALL TRUE)

# Unit tests
add_executable(polymarket_tests
    unit/crypto_test.cpp
    unit/eip712_test.cpp
    unit/types_test.cpp
    unit/order_builder_test.cpp
    unit/us_client_test.cpp
    unit/ws_subscriber_test.cpp
    unit/clob_websocket_test.cpp
    unit/glaze_test.cpp
    unit/http_test.cpp
    unit/rate_retry_test.cpp
    unit/unsupported_clients_test.cpp
)

target_link_libraries(polymarket_tests PRIVATE
    polymarket_core
    polymarket_crypto
    polymarket_clob
    polymarket_us
    polymarket_http
    polymarket_gamma
    polymarket_data
    glaze::glaze
    Catch2::Catch2WithMain
)

target_compile_features(polymarket_tests PRIVATE cxx_std_23)

# us_cli_test exec's the polymarket-us-cli binary via popen; pass its
# resolved path so the test doesn't depend on cwd. Only wired when
# apps/ is being built (the default).
if(POLYMARKET_BUILD_APPS)
    target_sources(polymarket_tests PRIVATE unit/us_cli_test.cpp)
    target_compile_definitions(polymarket_tests PRIVATE
        POLYMARKET_US_CLI_PATH="$<TARGET_FILE:polymarket_us_cli>")
    add_dependencies(polymarket_tests polymarket_us_cli)
endif()

# Register tests with CTest
include(CTest)
include(Catch)
catch_discover_tests(polymarket_tests)

# Semantic benchmark; timing is enforced only for optimized, unsanitized builds.
add_executable(polymarket_parse_benchmark parse_benchmark.cpp)
target_link_libraries(polymarket_parse_benchmark PRIVATE
    polymarket_core
    polymarket_crypto
    glaze::glaze
)
target_compile_features(polymarket_parse_benchmark PRIVATE cxx_std_23)
add_test(NAME polymarket_parse_benchmark
         COMMAND polymarket_parse_benchmark $<$<OR:$<CONFIG:Debug>,$<BOOL:${POLYMARKET_ENABLE_SANITIZERS}>>:--no-timing>)
# Generous 30s wall-clock timeout. The benchmark itself completes in
# ~0.1s; the timeout exists to fail-fast on a pathological regression
# (e.g., quadratic parse) rather than hang CI for 6 hours.
set_tests_properties(polymarket_parse_benchmark PROPERTIES TIMEOUT 30)

add_executable(polymarket_wide_math_tests unit/wide_math_test.cpp)
target_link_libraries(polymarket_wide_math_tests PRIVATE Catch2::Catch2WithMain)
target_compile_features(polymarket_wide_math_tests PRIVATE cxx_std_23)
target_compile_definitions(polymarket_wide_math_tests PRIVATE
    POLYMARKET_WIDE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/fixtures/wide-math.tsv")
catch_discover_tests(polymarket_wide_math_tests)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_executable(polymarket_ws_probe ws_probe.cpp)
target_link_libraries(polymarket_ws_probe PRIVATE polymarket::polymarket)
add_test(NAME polymarket_ws_loopback
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_ws_loopback.py"
        $<TARGET_FILE:polymarket_ws_probe>)
set_tests_properties(polymarket_ws_loopback PROPERTIES TIMEOUT 90)

# Manual public-read check; never registered as a network-dependent test.
add_executable(polymarket_public_probe consumer/main.cpp)
target_link_libraries(polymarket_public_probe PRIVATE polymarket::polymarket)

if(UNIX)
    add_test(NAME polymarket_install_hooks
        COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/test_install_hooks.py")
endif()
