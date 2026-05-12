cmake_minimum_required(VERSION 3.20)
project(polymarket-cpp VERSION 0.3.1 LANGUAGES CXX)

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

# Aggressive optimization flags for Release builds. MSVC has its own
# Release defaults; only override for GCC/Clang.
if(NOT MSVC)
    set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")
endif()

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

# CPU-specific tuning. GCC/Clang only — MSVC uses /arch flags with
# different semantics so we leave its defaults untouched.
if(NOT MSVC)
    if(POLYMARKET_NATIVE_ARCH)
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -march=native")
        message(STATUS "Using -march=native for CPU-specific optimizations")
    else()
        set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} -mtune=generic")
    endif()
endif()

# Find dependencies
find_package(OpenSSL REQUIRED)
find_package(CURL REQUIRED)
if(NOT WIN32)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WEBSOCKETS REQUIRED IMPORTED_TARGET libwebsockets)
else()
    # Windows: libwebsockets shipped via vcpkg config-mode
    find_package(libwebsockets CONFIG REQUIRED)
endif()

# libsecp256k1 — Bitcoin Core's reference implementation. Provides the
# only correct ECDSA public-key recovery primitive on this stack
# (OpenSSL has none). Fetched at configure time so consumers don't
# need a system package; recovery + extrakeys modules enabled so
# both sign-recoverable and ecrecover paths are available.
include(FetchContent)
set(SECP256K1_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXHAUSTIVE_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_CTIME_TESTS OFF CACHE BOOL "" FORCE)
set(SECP256K1_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SECP256K1_ENABLE_MODULE_RECOVERY ON CACHE BOOL "" FORCE)
set(SECP256K1_INSTALL OFF CACHE BOOL "" FORCE)
set(SECP256K1_DISABLE_SHARED ON CACHE BOOL "" FORCE)
FetchContent_Declare(secp256k1
    GIT_REPOSITORY https://github.com/bitcoin-core/secp256k1.git
    GIT_TAG        v0.6.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(secp256k1)

# Glaze — JSON library (compile-time reflection, ~3-4x parse speedup
# over nlohmann on numeric-heavy payloads — the CLOB orderbook hot
# path). Used by the CLOB WebSocket message dispatcher + the
# polymarket_us_cli stdin/stdout JSON envelope. License: MIT.
# Migrated from nlohmann/json v3.11.3 on 2026-05-11; the previous
# `FetchContent_Declare(json ...)` dep is removed.
FetchContent_Declare(glaze
    GIT_REPOSITORY https://github.com/stephenberry/glaze.git
    GIT_TAG        v7.6.0
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(glaze)

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
    secp256k1
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
