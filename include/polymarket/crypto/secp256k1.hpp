#pragma once

/// @file secp256k1.hpp
/// @brief secp256k1 ECDSA signing for Ethereum signatures
///
/// Provides recoverable ECDSA signatures compatible with Ethereum's
/// ecrecover precompile. Uses OpenSSL for the underlying cryptographic
/// operations.

#include "polymarket/core/error.hpp"
#include "polymarket/core/types.hpp"
#include "polymarket/crypto/keccak.hpp"

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

namespace polymarket::crypto {

/// Ethereum signature (65 bytes: r[32] + s[32] + v[1])
struct Signature {
  std::array<std::uint8_t, 32> r;
  std::array<std::uint8_t, 32> s;
  std::uint8_t v; // Recovery ID (27 or 28 for Ethereum)

  /// Convert to hex string (130 chars + 0x prefix)
  [[nodiscard]] std::string to_hex() const;

  /// Parse from hex string (with or without 0x prefix)
  [[nodiscard]] static Result<Signature> from_hex(std::string_view hex);

  /// Get the 65-byte representation
  [[nodiscard]] std::array<std::uint8_t, 65> to_bytes() const;
};

/// secp256k1 private key (32 bytes)
class PrivateKey {
public:
  ~PrivateKey();

  // Non-copyable (contains sensitive data)
  PrivateKey(const PrivateKey &) = delete;
  PrivateKey &operator=(const PrivateKey &) = delete;

  // Moveable
  PrivateKey(PrivateKey &&other) noexcept;
  PrivateKey &operator=(PrivateKey &&other) noexcept;

  /// Create from raw 32-byte key
  [[nodiscard]] static Result<PrivateKey>
  from_bytes(std::span<const std::uint8_t, 32> bytes);

  /// Create from hex string (with or without 0x prefix)
  [[nodiscard]] static Result<PrivateKey> from_hex(std::string_view hex);

  /// Get the corresponding public key (uncompressed, 65 bytes)
  [[nodiscard]] Result<std::array<std::uint8_t, 65>> public_key() const;

  /// Get the corresponding Ethereum address
  [[nodiscard]] Result<Address> address() const;

  /// Sign a 32-byte message hash (not raw message!)
  /// Returns a recoverable signature with v = 27 or 28
  [[nodiscard]] Result<Signature> sign(const Keccak256Hash &message_hash) const;

  /// Sign raw data (hashes with keccak256 first)
  [[nodiscard]] Result<Signature>
  sign_message(std::span<const std::uint8_t> data) const;

private:
  PrivateKey();
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

/// Verify a signature against a message hash and address
/// Returns true if the signature was created by the private key corresponding
/// to the address
[[nodiscard]] Result<bool> verify_signature(const Keccak256Hash &message_hash,
                                            const Signature &signature,
                                            const Address &expected_address);

/// Recover the signer's address from a signature
[[nodiscard]] Result<Address> recover_address(const Keccak256Hash &message_hash,
                                              const Signature &signature);

} // namespace polymarket::crypto
