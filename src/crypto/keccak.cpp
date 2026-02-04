/// @file keccak.cpp
/// @brief Keccak-256 implementation (Ethereum variant)
///
/// Uses OpenSSL's EVP interface for the hash computation.
/// IMPORTANT: Ethereum uses original Keccak (0x01 padding), not SHA-3 (0x06 padding).

#include "polymarket/crypto/keccak.hpp"

#include <openssl/evp.h>
#include <openssl/err.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>

namespace polymarket::crypto {

namespace {

// Keccak-256 internal state
constexpr int KECCAK_ROUNDS = 24;
constexpr int KECCAK_RATE = 136;  // (1600 - 256*2) / 8 bytes for Keccak-256

// Round constants
constexpr std::uint64_t RC[24] = {
    0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808aULL,
    0x8000000080008000ULL, 0x000000000000808bULL, 0x0000000080000001ULL,
    0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008aULL,
    0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000aULL,
    0x000000008000808bULL, 0x800000000000008bULL, 0x8000000000008089ULL,
    0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
    0x000000000000800aULL, 0x800000008000000aULL, 0x8000000080008081ULL,
    0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};

// Rotation offsets
constexpr int ROTATION[5][5] = {
    { 0,  1, 62, 28, 27},
    {36, 44,  6, 55, 20},
    { 3, 10, 43, 25, 39},
    {41, 45, 15, 21,  8},
    {18,  2, 61, 56, 14}
};

inline std::uint64_t rotl64(std::uint64_t x, int n) {
    return (x << n) | (x >> (64 - n));
}

void keccak_f(std::uint64_t state[25]) {
    for (int round = 0; round < KECCAK_ROUNDS; ++round) {
        // θ step
        std::uint64_t C[5], D[5];
        for (int x = 0; x < 5; ++x) {
            C[x] = state[x] ^ state[x + 5] ^ state[x + 10] ^ state[x + 15] ^ state[x + 20];
        }
        for (int x = 0; x < 5; ++x) {
            D[x] = C[(x + 4) % 5] ^ rotl64(C[(x + 1) % 5], 1);
        }
        for (int x = 0; x < 5; ++x) {
            for (int y = 0; y < 5; ++y) {
                state[x + 5 * y] ^= D[x];
            }
        }

        // ρ and π steps
        std::uint64_t B[25];
        for (int x = 0; x < 5; ++x) {
            for (int y = 0; y < 5; ++y) {
                int new_x = y;
                int new_y = (2 * x + 3 * y) % 5;
                B[new_x + 5 * new_y] = rotl64(state[x + 5 * y], ROTATION[y][x]);
            }
        }

        // χ step
        for (int x = 0; x < 5; ++x) {
            for (int y = 0; y < 5; ++y) {
                state[x + 5 * y] = B[x + 5 * y] ^ ((~B[(x + 1) % 5 + 5 * y]) & B[(x + 2) % 5 + 5 * y]);
            }
        }

        // ι step
        state[0] ^= RC[round];
    }
}

} // anonymous namespace

Keccak256Hash keccak256(std::span<const std::uint8_t> data) {
    // Initialize state to zero
    std::uint64_t state[25] = {0};
    
    // Absorb phase
    size_t offset = 0;
    while (offset + KECCAK_RATE <= data.size()) {
        for (int i = 0; i < KECCAK_RATE / 8; ++i) {
            std::uint64_t lane = 0;
            for (int j = 0; j < 8; ++j) {
                lane |= static_cast<std::uint64_t>(data[offset + i * 8 + j]) << (j * 8);
            }
            state[i] ^= lane;
        }
        keccak_f(state);
        offset += KECCAK_RATE;
    }

    // Absorb remaining bytes with padding
    std::array<std::uint8_t, KECCAK_RATE> padded = {0};
    size_t remaining = data.size() - offset;
    std::memcpy(padded.data(), data.data() + offset, remaining);
    
    // Keccak padding: 0x01 at end of message, 0x80 at end of block
    // IMPORTANT: Ethereum uses 0x01, NOT SHA-3's 0x06
    padded[remaining] = 0x01;
    padded[KECCAK_RATE - 1] |= 0x80;

    for (int i = 0; i < KECCAK_RATE / 8; ++i) {
        std::uint64_t lane = 0;
        for (int j = 0; j < 8; ++j) {
            lane |= static_cast<std::uint64_t>(padded[i * 8 + j]) << (j * 8);
        }
        state[i] ^= lane;
    }
    keccak_f(state);

    // Squeeze phase - extract 256 bits (32 bytes)
    Keccak256Hash hash;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 8; ++j) {
            hash[i * 8 + j] = static_cast<std::uint8_t>(state[i] >> (j * 8));
        }
    }
    
    return hash;
}

std::string hash_to_hex(const Keccak256Hash& hash) {
    std::ostringstream oss;
    oss << "0x";
    for (auto b : hash) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(b);
    }
    return oss.str();
}

Result<Keccak256Hash> hash_from_hex(std::string_view hex) {
    // Skip 0x prefix
    if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
        hex = hex.substr(2);
    }

    if (hex.size() != 64) {
        return std::unexpected(Error::validation("Hash must be 32 bytes (64 hex chars)"));
    }

    Keccak256Hash hash;
    for (size_t i = 0; i < 32; ++i) {
        char hi = hex[i * 2];
        char lo = hex[i * 2 + 1];
        
        auto nibble = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };

        int hi_val = nibble(hi);
        int lo_val = nibble(lo);
        if (hi_val < 0 || lo_val < 0) {
            return std::unexpected(Error::validation("Invalid hex character"));
        }
        hash[i] = static_cast<std::uint8_t>((hi_val << 4) | lo_val);
    }

    return hash;
}

Address address_from_pubkey(std::span<const std::uint8_t> pubkey) {
    // Public key should be 65 bytes (0x04 prefix + 64 bytes)
    // Or 64 bytes without prefix
    std::span<const std::uint8_t> key_bytes;
    if (pubkey.size() == 65 && pubkey[0] == 0x04) {
        key_bytes = pubkey.subspan(1);
    } else if (pubkey.size() == 64) {
        key_bytes = pubkey;
    } else {
        // Return zero address for invalid input
        return Address{};
    }

    // Hash the public key (without prefix)
    Keccak256Hash hash = keccak256(key_bytes);

    // Address is last 20 bytes of hash
    Address addr;
    std::copy(hash.begin() + 12, hash.end(), addr.bytes.begin());
    return addr;
}

std::string to_checksum_address(const Address& addr) {
    // Get lowercase hex without 0x
    std::string hex;
    hex.reserve(40);
    for (auto b : addr.bytes) {
        static const char* digits = "0123456789abcdef";
        hex += digits[(b >> 4) & 0x0f];
        hex += digits[b & 0x0f];
    }

    // Hash the lowercase address
    Keccak256Hash hash = keccak256(hex);

    // Apply EIP-55 checksum
    std::string result = "0x";
    result.reserve(42);
    for (size_t i = 0; i < 40; ++i) {
        // Get the corresponding nibble from the hash
        std::uint8_t hash_byte = hash[i / 2];
        std::uint8_t hash_nibble = (i % 2 == 0) ? (hash_byte >> 4) : (hash_byte & 0x0f);
        
        // Uppercase if hash nibble >= 8
        char c = hex[i];
        if (hash_nibble >= 8 && c >= 'a' && c <= 'f') {
            c = static_cast<char>(c - 'a' + 'A');
        }
        result += c;
    }

    return result;
}

} // namespace polymarket::crypto
