#pragma once

/// @file hmac.hpp
/// @brief HMAC-SHA256 for L2 API authentication
///
/// Provides HMAC-SHA256 signing with URL-safe base64 encoding
/// for Polymarket L2 (API key) authentication.

#include "polymarket/core/error.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace polymarket::crypto {

/// HMAC-SHA256 result (32 bytes)
using HmacSha256Hash = std::array<std::uint8_t, 32>;

/// Compute HMAC-SHA256
///
/// @param key Secret key
/// @param message Message to authenticate
/// @return 32-byte HMAC
[[nodiscard]] Result<HmacSha256Hash>
hmac_sha256(std::span<const std::uint8_t> key,
            std::span<const std::uint8_t> message);

/// Compute HMAC-SHA256 with string inputs
[[nodiscard]] inline Result<HmacSha256Hash>
hmac_sha256(std::string_view key, std::string_view message) {
  return hmac_sha256(
      std::span{reinterpret_cast<const std::uint8_t *>(key.data()), key.size()},
      std::span{reinterpret_cast<const std::uint8_t *>(message.data()),
                message.size()});
}

/// Encode bytes to URL-safe base64 (RFC 4648 section 5)
/// Uses - instead of + and _ instead of /, no padding
[[nodiscard]] std::string base64url_encode(std::span<const std::uint8_t> data);

/// Decode URL-safe base64 to bytes
[[nodiscard]] Result<std::vector<std::uint8_t>>
base64url_decode(std::string_view encoded);

/// Standard base64 encode (RFC 4648 section 4)
[[nodiscard]] std::string base64_encode(std::span<const std::uint8_t> data);

/// Standard base64 decode
[[nodiscard]] Result<std::vector<std::uint8_t>>
base64_decode(std::string_view encoded);

/// Generate L2 signature for API requests
///
/// The signature is computed as:
///   base64url(hmac_sha256(secret, timestamp + method + path + body))
///
/// @param api_secret Base64-encoded API secret
/// @param timestamp Unix timestamp string
/// @param method HTTP method (GET, POST, DELETE)
/// @param path Request path including query string
/// @param body Request body (empty for GET/DELETE)
/// @return URL-safe base64 encoded signature
[[nodiscard]] Result<std::string>
generate_l2_signature(std::string_view api_secret, std::string_view timestamp,
                      std::string_view method, std::string_view path,
                      std::string_view body = "");

/// Generate Builder signature (same format as L2)
[[nodiscard]] inline Result<std::string>
generate_builder_signature(std::string_view builder_secret,
                           std::string_view timestamp, std::string_view method,
                           std::string_view path, std::string_view body = "") {
  return generate_l2_signature(builder_secret, timestamp, method, path, body);
}

} // namespace polymarket::crypto
