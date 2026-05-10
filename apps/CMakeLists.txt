# Production CLI binaries built on top of the polymarket library.
#
# These differ from `examples/` in that they are intended to ship in
# downstream service Docker images (e.g., polymarket-trader's Python
# subprocess wrapper). Examples remain operator-facing scripts; apps
# are part of the platform's runtime surface.

# nlohmann/json — provided by the root CMakeLists.txt FetchContent
# (target `nlohmann_json::nlohmann_json` is global). v0.3.1 collapsed
# the previous apps-only FetchContent_Declare to avoid a duplicate
# `nlohmann_json` target collision now that the root scope owns it.

add_executable(polymarket_us_cli polymarket_us_cli.cpp)
target_link_libraries(polymarket_us_cli
    PRIVATE polymarket nlohmann_json::nlohmann_json)
target_compile_features(polymarket_us_cli PRIVATE cxx_std_23)
set_target_properties(polymarket_us_cli PROPERTIES
    OUTPUT_NAME "polymarket-us-cli")

install(TARGETS polymarket_us_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
