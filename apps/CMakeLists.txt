# Production CLI binaries built on top of the polymarket library.
#
# These differ from `examples/` in that they are intended to ship in
# downstream service Docker images (e.g., polymarket-trader's Python
# subprocess wrapper). Examples remain operator-facing scripts; apps
# are part of the platform's runtime surface.

include(FetchContent)

# nlohmann/json — JSON parse/build for the CLI's stdin/stdout
# protocol. Scoped to apps/ only; not added to the library's public
# dependencies. cpp17-compatible single header; downloaded once via
# FetchContent so we don't take a system dep.
FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
    GIT_SHALLOW TRUE)
set(JSON_BuildTests OFF CACHE INTERNAL "")
set(JSON_Install OFF CACHE INTERNAL "")
FetchContent_MakeAvailable(nlohmann_json)

add_executable(polymarket_us_cli polymarket_us_cli.cpp)
target_link_libraries(polymarket_us_cli
    PRIVATE polymarket nlohmann_json::nlohmann_json)
target_compile_features(polymarket_us_cli PRIVATE cxx_std_23)
set_target_properties(polymarket_us_cli PROPERTIES
    OUTPUT_NAME "polymarket-us-cli")

install(TARGETS polymarket_us_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
