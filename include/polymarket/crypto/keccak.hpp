#pragma once

/// @file keccak.hpp
/// @brief Keccak-256 hash implementation (Ethereum variant)
///
/// IMPORTANT: Ethereum uses the original Keccak submission (padding 0x01),
/// NOT the NIST SHA-3 standard (padding 0x06). This implementation provides
/// the Ethereum-compatible Keccak-256 hash function.

#include "polymarket/core/error.hpp"
#include "polymarket/core/types.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace polymarket::crypto {

/// Keccak-256 hash result (32 bytes)
using Keccak256Hash = std::array<std::uint8_t, 32>;

/// Compute Keccak-256 hash (Ethereum variant, NOT SHA-3)
///
/// @param data Input data to hash
/// @return 32-byte Keccak-256 hash
[[nodiscard]] Keccak256Hash keccak256(std::span<const std::uint8_t> data);

/// Compute Keccak-256 hash from string
[[nodiscard]] inline Keccak256Hash keccak256(std::string_view data) {
  return keccak256(std::span{
      reinterpret_cast<const std::uint8_t *>(data.data()), data.size()});
}

/// Compute Keccak-256 hash from vector
[[nodiscard]] inline Keccak256Hash
keccak256(const std::vector<std::uint8_t> &data) {
  return keccak256(std::span{data});
}

/// Convert hash to hex string (with 0x prefix)
[[nodiscard]] std::string hash_to_hex(const Keccak256Hash &hash);

/// Convert hex string to hash (with or without 0x prefix)
[[nodiscard]] Result<Keccak256Hash> hash_from_hex(std::string_view hex);

/// Derive Ethereum address from uncompressed public key (65 bytes with 0x04
/// prefix) Address = keccak256(pubkey[1:])[12:]
[[nodiscard]] Address address_from_pubkey(std::span<const std::uint8_t> pubkey);

/// Compute EIP-55 checksum address
[[nodiscard]] std::string to_checksum_address(const Address &addr);

} // namespace polymarket::crypto
