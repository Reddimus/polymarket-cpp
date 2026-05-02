#pragma once

/// @file version.hpp
/// @brief Polymarket C++ SDK version information

#define POLYMARKET_VERSION_MAJOR 0
#define POLYMARKET_VERSION_MINOR 1
#define POLYMARKET_VERSION_PATCH 2
#define POLYMARKET_VERSION_STRING "0.1.2"

namespace polymarket {

/// Get the SDK version string
constexpr const char *version() noexcept { return POLYMARKET_VERSION_STRING; }

/// Get the major version number
constexpr int version_major() noexcept { return POLYMARKET_VERSION_MAJOR; }

/// Get the minor version number
constexpr int version_minor() noexcept { return POLYMARKET_VERSION_MINOR; }

/// Get the patch version number
constexpr int version_patch() noexcept { return POLYMARKET_VERSION_PATCH; }

} // namespace polymarket
