# Examples — binaries are named `example_<topic>` so the project-root
# Makefile's `run-<topic>` targets resolve to `./build/examples/example_<topic>`.
# This matches the convention used across the sibling C++ SDK repos
# (kalshi-cpp / nws-cpp / ncei-cpp / open-meteo-cpp).
if(POLYMARKET_BUILD_EXAMPLES)
    add_executable(example_market_data market_data.cpp)
    target_link_libraries(example_market_data PRIVATE polymarket)
    target_compile_features(example_market_data PRIVATE cxx_std_23)

    add_executable(example_order_placement order_placement.cpp)
    target_link_libraries(example_order_placement PRIVATE polymarket)
    target_compile_features(example_order_placement PRIVATE cxx_std_23)
endif()
