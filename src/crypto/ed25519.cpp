/// @file ed25519.cpp
/// @brief Ed25519 signing for Polymarket US API

#include "polymarket/crypto/ed25519.hpp"
#include "polymarket/crypto/hmac.hpp"

#include <openssl/evp.h>

#include <algorithm>
#include <chrono>
#include <cstring>

namespace polymarket::crypto {

// =========================================================================
// Ed25519PrivateKey Implementation
// =========================================================================

struct Ed25519PrivateKey::Impl {
    EVP_PKEY* key{nullptr};

    ~Impl() {
        if (key) {
            EVP_PKEY_free(key);
        }
    }
};

Ed25519PrivateKey::Ed25519PrivateKey() : impl_(std::make_unique<Impl>()) {}

Ed25519PrivateKey::~Ed25519PrivateKey() = default;

Ed25519PrivateKey::Ed25519PrivateKey(Ed25519PrivateKey&& other) noexcept = default;
Ed25519PrivateKey& Ed25519PrivateKey::operator=(Ed25519PrivateKey&& other) noexcept = default;

Result<Ed25519PrivateKey> Ed25519PrivateKey::from_seed(std::span<const std::uint8_t, 32> seed) {
    Ed25519PrivateKey key;

    key.impl_->key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr,
                                                   seed.data(), seed.size());
    if (!key.impl_->key) {
        return std::unexpected(Error::crypto("Failed to create Ed25519 key from seed"));
    }

    return key;
}

Result<Ed25519PrivateKey> Ed25519PrivateKey::from_hex(std::string_view hex) {
    // Skip 0x prefix
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex = hex.substr(2);
    }

    if (hex.size() != 64) {
        return std::unexpected(Error::validation("Ed25519 seed must be 32 bytes (64 hex chars)"));
    }

    std::array<std::uint8_t, 32> seed;
    for (size_t i = 0; i < 32; ++i) {
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        int hi = nibble(hex[i * 2]);
        int lo = nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) {
            return std::unexpected(Error::validation("Invalid hex character"));
        }
        seed[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }

    return from_seed(seed);
}

Result<std::array<std::uint8_t, 32>> Ed25519PrivateKey::public_key() const {
    if (!impl_->key) {
        return std::unexpected(Error::crypto("Ed25519 key not initialized"));
    }

    std::array<std::uint8_t, 32> pubkey;
    size_t len = 32;

    if (EVP_PKEY_get_raw_public_key(impl_->key, pubkey.data(), &len) != 1 || len != 32) {
        return std::unexpected(Error::crypto("Failed to get Ed25519 public key"));
    }

    return pubkey;
}

Result<Ed25519Signature> Ed25519PrivateKey::sign(std::span<const std::uint8_t> message) const {
    if (!impl_->key) {
        return std::unexpected(Error::crypto("Ed25519 key not initialized"));
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return std::unexpected(Error::crypto("Failed to create signing context"));
    }

    Ed25519Signature sig;
    size_t sig_len = 64;

    int result = EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, impl_->key);
    if (result != 1) {
        EVP_MD_CTX_free(ctx);
        return std::unexpected(Error::crypto("Failed to init Ed25519 signing"));
    }

    result = EVP_DigestSign(ctx, sig.data(), &sig_len, message.data(), message.size());
    EVP_MD_CTX_free(ctx);

    if (result != 1 || sig_len != 64) {
        return std::unexpected(Error::crypto("Ed25519 signing failed"));
    }

    return sig;
}

// =========================================================================
// Verification
// =========================================================================

Result<bool> ed25519_verify(
    std::span<const std::uint8_t> message,
    const Ed25519Signature& signature,
    std::span<const std::uint8_t, 32> public_key) {
    
    EVP_PKEY* pkey = EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr,
                                                  public_key.data(), public_key.size());
    if (!pkey) {
        return std::unexpected(Error::crypto("Failed to create Ed25519 public key"));
    }

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        EVP_PKEY_free(pkey);
        return std::unexpected(Error::crypto("Failed to create verification context"));
    }

    int result = EVP_DigestVerifyInit(ctx, nullptr, nullptr, nullptr, pkey);
    if (result != 1) {
        EVP_MD_CTX_free(ctx);
        EVP_PKEY_free(pkey);
        return std::unexpected(Error::crypto("Failed to init Ed25519 verification"));
    }

    result = EVP_DigestVerify(ctx, signature.data(), signature.size(),
                               message.data(), message.size());
    
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(pkey);

    return result == 1;
}

// =========================================================================
// US API Authentication
// =========================================================================

Result<std::pair<std::string, std::string>> generate_us_auth_headers(
    std::string_view key_id,
    std::string_view secret_key,
    std::string_view method,
    std::string_view path) {
    
    // Get current timestamp
    auto now = std::chrono::system_clock::now();
    auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
        now.time_since_epoch()).count();
    std::string ts_str = std::to_string(timestamp);

    // Decode the base64 secret key
    auto secret_bytes = base64_decode(secret_key);
    if (!secret_bytes) {
        return std::unexpected(secret_bytes.error());
    }

    // Create Ed25519 key from the decoded secret
    if (secret_bytes->size() < 32) {
        return std::unexpected(Error::validation("Secret key too short"));
    }

    std::array<std::uint8_t, 32> seed;
    std::copy_n(secret_bytes->begin(), 32, seed.begin());

    auto ed_key = Ed25519PrivateKey::from_seed(seed);
    if (!ed_key) {
        return std::unexpected(ed_key.error());
    }

    // Build message to sign: timestamp + method + path
    std::string message = ts_str + std::string(method) + std::string(path);

    // Sign the message
    auto sig = ed_key->sign(message);
    if (!sig) {
        return std::unexpected(sig.error());
    }

    // Base64 encode the signature
    std::string sig_b64 = base64_encode(*sig);

    return std::make_pair(ts_str, sig_b64);
}

} // namespace polymarket::crypto
