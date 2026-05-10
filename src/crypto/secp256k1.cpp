/// @file secp256k1.cpp
/// @brief secp256k1 ECDSA signing implementation
///
/// Key derivation + public-key serialization use OpenSSL (we already
/// link it for HMAC + Ed25519). The signing and recovery paths use
/// libsecp256k1 — Bitcoin Core's reference implementation — because:
///
///   1. OpenSSL ECDSA does not produce a recovery id, so the previous
///      implementation tried to guess v from a parity heuristic
///      (``r[31] ^ message_hash[0] & 1``). That guess is wrong ~50%
///      of the time and made ecrecover() unreliable. libsecp256k1's
///      ``secp256k1_ecdsa_sign_recoverable`` returns the correct
///      recid alongside the signature.
///
///   2. OpenSSL has no public-key recovery primitive at all, so
///      ``recover_address`` could only return an error. libsecp256k1's
///      ``secp256k1_ecdsa_recover`` is the canonical primitive used by
///      every Ethereum implementation.

#include "polymarket/crypto/secp256k1.hpp"

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/obj_mac.h>

#include <secp256k1.h>
#include <secp256k1_recovery.h>

#include <algorithm>
#include <cstring>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace polymarket::crypto {

namespace {

/// Process-wide signing/verification context. Building one is
/// somewhat expensive (table precomputation); the secp256k1 maintainers
/// recommend reusing a single context for the lifetime of the process.
/// Thread-safe for use after construction; randomization is optional
/// per the upstream API contract for ``SECP256K1_CONTEXT_SIGN`` |
/// ``SECP256K1_CONTEXT_VERIFY``.
secp256k1_context *secp_ctx() {
  static secp256k1_context *ctx = []() {
    return secp256k1_context_create(SECP256K1_CONTEXT_SIGN |
                                    SECP256K1_CONTEXT_VERIFY);
  }();
  return ctx;
}

} // namespace

// =========================================================================
// Signature Implementation
// =========================================================================

std::string Signature::to_hex() const {
  std::ostringstream oss;
  oss << "0x";
  for (auto b : r) {
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
  }
  for (auto b : s) {
    oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
  }
  oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(v);
  return oss.str();
}

Result<Signature> Signature::from_hex(std::string_view hex) {
  // Skip 0x prefix
  if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
    hex = hex.substr(2);
  }

  if (hex.size() != 130) {
    return std::unexpected(
        Error::validation("Signature must be 65 bytes (130 hex chars)"));
  }

  auto nibble = [](char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };

  Signature sig;

  // Parse r (32 bytes = 64 hex chars)
  for (size_t i = 0; i < 32; ++i) {
    int hi = nibble(hex[i * 2]);
    int lo = nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return std::unexpected(
          Error::validation("Invalid hex character in signature"));
    }
    sig.r[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }

  // Parse s (32 bytes = 64 hex chars)
  for (size_t i = 0; i < 32; ++i) {
    int hi = nibble(hex[64 + i * 2]);
    int lo = nibble(hex[64 + i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return std::unexpected(
          Error::validation("Invalid hex character in signature"));
    }
    sig.s[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }

  // Parse v (1 byte = 2 hex chars)
  int hi = nibble(hex[128]);
  int lo = nibble(hex[129]);
  if (hi < 0 || lo < 0) {
    return std::unexpected(
        Error::validation("Invalid hex character in signature"));
  }
  sig.v = static_cast<std::uint8_t>((hi << 4) | lo);

  return sig;
}

std::array<std::uint8_t, 65> Signature::to_bytes() const {
  std::array<std::uint8_t, 65> bytes;
  std::copy(r.begin(), r.end(), bytes.begin());
  std::copy(s.begin(), s.end(), bytes.begin() + 32);
  bytes[64] = v;
  return bytes;
}

// =========================================================================
// PrivateKey Implementation
// =========================================================================

struct PrivateKey::Impl {
  EVP_PKEY *key{nullptr};
  std::array<std::uint8_t, 32> raw_key{};

  ~Impl() {
    if (key) {
      EVP_PKEY_free(key);
    }
    // Securely erase the raw key
    std::fill(raw_key.begin(), raw_key.end(), std::uint8_t{0});
  }
};

PrivateKey::PrivateKey() : impl_(std::make_unique<Impl>()) {}

PrivateKey::~PrivateKey() = default;

PrivateKey::PrivateKey(PrivateKey &&other) noexcept = default;
PrivateKey &PrivateKey::operator=(PrivateKey &&other) noexcept = default;

Result<PrivateKey>
PrivateKey::from_bytes(std::span<const std::uint8_t, 32> bytes) {
  PrivateKey key;
  std::copy(bytes.begin(), bytes.end(), key.impl_->raw_key.begin());

  // Create EC key from raw bytes using EVP interface
  BIGNUM *priv_bn = BN_bin2bn(bytes.data(), 32, nullptr);
  if (!priv_bn) {
    return std::unexpected(
        Error::crypto("Failed to create BIGNUM from private key"));
  }

  // Create EVP_PKEY from the private key
  EVP_PKEY *pkey = EVP_PKEY_new();
  if (!pkey) {
    BN_free(priv_bn);
    return std::unexpected(Error::crypto("Failed to create EVP_PKEY"));
  }

  // Create EC_KEY and set the private key
  EC_KEY *ec_key = EC_KEY_new_by_curve_name(NID_secp256k1);
  if (!ec_key) {
    EVP_PKEY_free(pkey);
    BN_free(priv_bn);
    return std::unexpected(
        Error::crypto("Failed to create EC_KEY for secp256k1"));
  }

  if (EC_KEY_set_private_key(ec_key, priv_bn) != 1) {
    EC_KEY_free(ec_key);
    EVP_PKEY_free(pkey);
    BN_free(priv_bn);
    return std::unexpected(Error::crypto("Failed to set private key"));
  }

  // Compute public key from private key
  const EC_GROUP *group = EC_KEY_get0_group(ec_key);
  EC_POINT *pub_point = EC_POINT_new(group);
  if (!pub_point ||
      EC_POINT_mul(group, pub_point, priv_bn, nullptr, nullptr, nullptr) != 1) {
    EC_POINT_free(pub_point);
    EC_KEY_free(ec_key);
    EVP_PKEY_free(pkey);
    BN_free(priv_bn);
    return std::unexpected(Error::crypto("Failed to compute public key"));
  }

  if (EC_KEY_set_public_key(ec_key, pub_point) != 1) {
    EC_POINT_free(pub_point);
    EC_KEY_free(ec_key);
    EVP_PKEY_free(pkey);
    BN_free(priv_bn);
    return std::unexpected(Error::crypto("Failed to set public key"));
  }

  EC_POINT_free(pub_point);
  BN_free(priv_bn);

  if (EVP_PKEY_assign_EC_KEY(pkey, ec_key) != 1) {
    EC_KEY_free(ec_key);
    EVP_PKEY_free(pkey);
    return std::unexpected(
        Error::crypto("Failed to assign EC_KEY to EVP_PKEY"));
  }

  key.impl_->key = pkey;
  return key;
}

Result<PrivateKey> PrivateKey::from_hex(std::string_view hex) {
  // Skip 0x prefix
  if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
    hex = hex.substr(2);
  }

  if (hex.size() != 64) {
    return std::unexpected(
        Error::validation("Private key must be 32 bytes (64 hex chars)"));
  }

  std::array<std::uint8_t, 32> bytes;
  for (size_t i = 0; i < 32; ++i) {
    auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9')
        return c - '0';
      if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
      if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
      return -1;
    };
    int hi = nibble(hex[i * 2]);
    int lo = nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return std::unexpected(Error::validation("Invalid hex character"));
    }
    bytes[i] = static_cast<std::uint8_t>((hi << 4) | lo);
  }

  return from_bytes(bytes);
}

Result<std::array<std::uint8_t, 65>> PrivateKey::public_key() const {
  if (!impl_->key) {
    return std::unexpected(Error::crypto("Private key not initialized"));
  }

  const EC_KEY *ec_key = EVP_PKEY_get0_EC_KEY(impl_->key);
  if (!ec_key) {
    return std::unexpected(Error::crypto("Failed to get EC_KEY"));
  }

  const EC_POINT *pub_point = EC_KEY_get0_public_key(ec_key);
  const EC_GROUP *group = EC_KEY_get0_group(ec_key);

  std::array<std::uint8_t, 65> pubkey;
  size_t len =
      EC_POINT_point2oct(group, pub_point, POINT_CONVERSION_UNCOMPRESSED,
                         pubkey.data(), pubkey.size(), nullptr);
  if (len != 65) {
    return std::unexpected(Error::crypto("Failed to serialize public key"));
  }

  return pubkey;
}

Result<Address> PrivateKey::address() const {
  auto pubkey = public_key();
  if (!pubkey) {
    return std::unexpected(pubkey.error());
  }
  return address_from_pubkey(*pubkey);
}

Result<Signature> PrivateKey::sign(const Keccak256Hash &message_hash) const {
  if (!impl_->key) {
    return std::unexpected(Error::crypto("Private key not initialized"));
  }

  // Sign with libsecp256k1 — returns (r, s) plus a recid in {0, 1}
  // that is provably correct (matches the y-parity of the ephemeral
  // point). RFC6979 deterministic-k is the default nonce function;
  // signing the same hash with the same key always produces the
  // same signature (good for reproducibility tests).
  secp256k1_ecdsa_recoverable_signature recoverable;
  if (secp256k1_ecdsa_sign_recoverable(
          secp_ctx(), &recoverable, message_hash.data(), impl_->raw_key.data(),
          /* noncefp */ nullptr,
          /* ndata   */ nullptr) != 1) {
    return std::unexpected(
        Error::crypto("secp256k1_ecdsa_sign_recoverable failed"));
  }

  std::array<std::uint8_t, 64> compact{};
  int recid = -1;
  if (secp256k1_ecdsa_recoverable_signature_serialize_compact(
          secp_ctx(), compact.data(), &recid, &recoverable) != 1) {
    return std::unexpected(
        Error::crypto("Failed to serialize recoverable signature"));
  }
  if (recid != 0 && recid != 1) {
    // libsecp256k1 only ever returns 0 or 1 for the low-s normalized
    // path used here, but be defensive: a recid of 2 or 3 would
    // require x-coordinate overflow (vanishingly rare) and Ethereum's
    // ecrecover doesn't accept it.
    return std::unexpected(Error::crypto("Unsupported recid"));
  }

  Signature result;
  std::copy_n(compact.begin(), 32, result.r.begin());
  std::copy_n(compact.begin() + 32, 32, result.s.begin());
  // Pre-EIP-155 mapping: v = 27 + recid. Polymarket EIP-712 signatures
  // and the rest of the SDK (clob/order_builder.cpp) consume this form.
  result.v = static_cast<std::uint8_t>(27 + recid);
  return result;
}

Result<Signature>
PrivateKey::sign_message(std::span<const std::uint8_t> data) const {
  Keccak256Hash hash = keccak256(data);
  return sign(hash);
}

// =========================================================================
// Recovery Functions
// =========================================================================

Result<bool> verify_signature(const Keccak256Hash &message_hash,
                              const Signature &signature,
                              const Address &expected_address) {

  auto recovered = recover_address(message_hash, signature);
  if (!recovered) {
    return std::unexpected(recovered.error());
  }
  return *recovered == expected_address;
}

Result<Address> recover_address(const Keccak256Hash &message_hash,
                                const Signature &signature) {

  // Pre-EIP-155 v ∈ {27, 28}. EIP-155 packs chain_id but Polymarket's
  // EIP-712 typed-data signatures use the legacy form throughout, so
  // anything outside {27, 28} is malformed for this code path.
  int recid = static_cast<int>(signature.v);
  if (recid >= 27) {
    recid -= 27;
  }
  if (recid != 0 && recid != 1) {
    return std::unexpected(Error::crypto("Invalid recovery ID"));
  }

  // Reassemble the recoverable signature from (r, s, recid).
  std::array<std::uint8_t, 64> compact{};
  std::copy(signature.r.begin(), signature.r.end(), compact.begin());
  std::copy(signature.s.begin(), signature.s.end(), compact.begin() + 32);

  secp256k1_ecdsa_recoverable_signature recoverable;
  if (secp256k1_ecdsa_recoverable_signature_parse_compact(
          secp_ctx(), &recoverable, compact.data(), recid) != 1) {
    return std::unexpected(
        Error::crypto("Failed to parse recoverable signature"));
  }

  secp256k1_pubkey pubkey;
  if (secp256k1_ecdsa_recover(secp_ctx(), &pubkey, &recoverable,
                              message_hash.data()) != 1) {
    return std::unexpected(Error::crypto("secp256k1_ecdsa_recover failed"));
  }

  // Serialize uncompressed (0x04 + 64 bytes); address_from_pubkey
  // accepts either the leading-0x04 form or the bare 64-byte form
  // (it strips the prefix internally).
  std::array<std::uint8_t, 65> serialized{};
  std::size_t serialized_len = serialized.size();
  if (secp256k1_ec_pubkey_serialize(secp_ctx(), serialized.data(),
                                    &serialized_len, &pubkey,
                                    SECP256K1_EC_UNCOMPRESSED) != 1 ||
      serialized_len != 65) {
    return std::unexpected(
        Error::crypto("Failed to serialize recovered pubkey"));
  }

  return address_from_pubkey(serialized);
}

} // namespace polymarket::crypto
