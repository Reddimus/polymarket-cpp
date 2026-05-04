cmake_minimum_required(VERSION 3.20)
project(polymarket-cpp VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Options
option(POLYMARKET_BUILD_TESTS "Build tests" ON)
option(POLYMARKET_BUILD_EXAMPLES "Build examples" ON)
option(POLYMARKET_BUILD_APPS "Build production CLI binaries (apps/)" ON)
option(POLYMARKET_ENABLE_LTO "Enable Link Time Optimization" ON)
option(POLYMARKET_NATIVE_ARCH "Use -march=native for CPU-specific tuning" OFF)
option(POLYMARKET_USE_CPP26 "Enable C++26 features (experimental)" OFF)

# C++26 support (experimental)
if(POLYMARKET_USE_CPP26)
    set(CMAKE_CXX_STANDARD 26)
    add_compile_definitions(POLYMARKET_HAS_REFLECTION)
    message(STATUS "C++26 mode enabled (experimental)")
endif()

# Aggressive optimization flags for Release builds
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Enable LTO if requested and supported
if(POLYMARKET_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT LTO_SUPPORTED OUTPUT LTO_ERROR)
    if(LTO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
        message(STATUS "LTO enabled for Release builds")
    else()
        message(WARNING "LTO not supported: ${LTO_ERROR}")
    endif()
endif()

# CPU-specific tuning (optional, not portable)
if(POLYMARKET_NATIVE_ARCH)
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=native")
    message(STATUS "Using -march=native for CPU-specific optimizations")
else()
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mtune=generic")
endif()

# Find dependencies
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(WEBSOCKETS REQUIRED IMPORTED_TARGET libwebsockets)

# Include directories
include_directories(${CMAKE_SOURCE_DIR}/include)

# Add subdirectories
add_subdirectory(src)

if(POLYMARKET_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(POLYMARKET_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(POLYMARKET_BUILD_APPS)
    add_subdirectory(apps)
endif()

# Install targets
set(POLYMARKET_INSTALL_CMAKEDIR "${CMAKE_INSTALL_LIBDIR}/cmake/polymarket")

install(TARGETS 
    polymarket_core 
    polymarket_http
    polymarket_crypto 
    polymarket_clob 
    polymarket_gamma 
    polymarket_data 
    polymarket_us 
    polymarket
    EXPORT polymarketTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(DIRECTORY include/polymarket DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/polymarketConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/polymarketConfig.cmake
    INSTALL_DESTINATION ${POLYMARKET_INSTALL_CMAKEDIR}
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/polymarketConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
export(EXPORT polymarketTargets
    FILE ${CMAKE_CURRENT_BINARY_DIR}/polymarketTargets.cmake
    NAMESPACE polymarket::
)
install(EXPORT polymarketTargets
    FILE polymarketTargets.cmake
    NAMESPACE polymarket::
    DESTINATION ${POLYMARKET_INSTALL_CMAKEDIR}
)
install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/polymarketConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/polymarketConfigVersion.cmake
    DESTINATION ${POLYMARKET_INSTALL_CMAKEDIR}
)
