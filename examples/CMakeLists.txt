# Examples
if(POLYMARKET_BUILD_EXAMPLES)
    add_executable(market_data market_data.cpp)
    target_link_libraries(market_data PRIVATE polymarket)
    target_compile_features(market_data PRIVATE cxx_std_23)

    add_executable(order_placement order_placement.cpp)
    target_link_libraries(order_placement PRIVATE polymarket)
    target_compile_features(order_placement PRIVATE cxx_std_23)
endif()
