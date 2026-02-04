#pragma once

/// @file ed25519.hpp
/// @brief Ed25519 signing for Polymarket US API
///
/// Provides Ed25519 digital signatures for the Polymarket US platform
/// which uses a different authentication scheme than the CLOB.

#include "polymarket/core/error.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace polymarket::crypto {

/// Ed25519 signature (64 bytes)
using Ed25519Signature = std::array<std::uint8_t, 64>;

/// Ed25519 private key
class Ed25519PrivateKey {
public:
    ~Ed25519PrivateKey();

    // Non-copyable
    Ed25519PrivateKey(const Ed25519PrivateKey&) = delete;
    Ed25519PrivateKey& operator=(const Ed25519PrivateKey&) = delete;

    // Moveable
    Ed25519PrivateKey(Ed25519PrivateKey&& other) noexcept;
    Ed25519PrivateKey& operator=(Ed25519PrivateKey&& other) noexcept;

    /// Create from raw 32-byte seed
    [[nodiscard]] static Result<Ed25519PrivateKey> from_seed(
        std::span<const std::uint8_t, 32> seed);

    /// Create from hex string (with or without 0x prefix)
    [[nodiscard]] static Result<Ed25519PrivateKey> from_hex(std::string_view hex);

    /// Get the public key (32 bytes)
    [[nodiscard]] Result<std::array<std::uint8_t, 32>> public_key() const;

    /// Sign a message
    [[nodiscard]] Result<Ed25519Signature> sign(std::span<const std::uint8_t> message) const;

    /// Sign a string message
    [[nodiscard]] Result<Ed25519Signature> sign(std::string_view message) const {
        return sign(std::span{reinterpret_cast<const std::uint8_t*>(message.data()),
                             message.size()});
    }

private:
    Ed25519PrivateKey();
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

/// Verify an Ed25519 signature
[[nodiscard]] Result<bool> ed25519_verify(
    std::span<const std::uint8_t> message,
    const Ed25519Signature& signature,
    std::span<const std::uint8_t, 32> public_key);

/// Generate authentication headers for Polymarket US API
///
/// @param key_id API key ID
/// @param secret_key Base64-encoded secret key
/// @param method HTTP method
/// @param path Request path
/// @return Tuple of (timestamp, signature) for headers
[[nodiscard]] Result<std::pair<std::string, std::string>> generate_us_auth_headers(
    std::string_view key_id,
    std::string_view secret_key,
    std::string_view method,
    std::string_view path);

} // namespace polymarket::crypto
