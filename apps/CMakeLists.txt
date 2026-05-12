# Production CLI binaries built on top of the polymarket library.
#
# These differ from `examples/` in that they are intended to ship in
# downstream service Docker images (e.g., polymarket-trader's Python
# subprocess wrapper). Examples remain operator-facing scripts; apps
# are part of the platform's runtime surface.

# Glaze — JSON library, provided by the root CMakeLists.txt
# FetchContent (target `glaze::glaze` is global, header-only).
# Migrated from nlohmann_json v3.11.3 on 2026-05-11.

add_executable(polymarket_us_cli polymarket_us_cli.cpp)
target_link_libraries(polymarket_us_cli
    PRIVATE polymarket glaze::glaze)
target_compile_features(polymarket_us_cli PRIVATE cxx_std_23)
set_target_properties(polymarket_us_cli PROPERTIES
    OUTPUT_NAME "polymarket-us-cli")

install(TARGETS polymarket_us_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
