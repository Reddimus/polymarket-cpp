/// @file crypto_test.cpp
/// @brief Unit tests for cryptographic primitives

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "polymarket/crypto/hmac.hpp"
#include "polymarket/crypto/keccak.hpp"
#include "polymarket/crypto/secp256k1.hpp"

using namespace polymarket;
using namespace polymarket::crypto;

TEST_CASE("Keccak-256 test vectors", "[crypto][keccak]") {
  SECTION("Empty string") {
    auto hash = keccak256("");
    // Expected:
    // c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470
    REQUIRE(hash[0] == 0xc5);
    REQUIRE(hash[1] == 0xd2);
    REQUIRE(hash[31] == 0x70);
  }

  SECTION("Hello World") {
    auto hash = keccak256("Hello World");
    // We can verify the output format
    REQUIRE(hash.size() == 32);
  }

  SECTION("Known Ethereum test vector") {
    // "abc" ->
    // 0x4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45
    auto hash = keccak256("abc");
    REQUIRE(hash[0] == 0x4e);
    REQUIRE(hash[1] == 0x03);
    REQUIRE(hash[2] == 0x65);
  }

  SECTION("Binary data") {
    std::array<uint8_t, 4> data = {0x01, 0x02, 0x03, 0x04};
    auto hash = keccak256(data);
    REQUIRE(hash.size() == 32);
  }
}

TEST_CASE("Address from public key", "[crypto][keccak]") {
  // Test vector from Ethereum
  // Public key (uncompressed, without 04 prefix) that corresponds to a known
  // address

  // We'll test that address_from_pubkey produces the expected 20 bytes
  std::array<uint8_t, 64> pubkey;
  std::fill(pubkey.begin(), pubkey.end(), 0x01); // Dummy pubkey for format test

  auto addr = address_from_pubkey(pubkey);
  REQUIRE(addr.size() == 20);
}

TEST_CASE("EIP-55 checksum addresses", "[crypto][keccak]") {
  SECTION("All lowercase") {
    Address addr =
        Address::from_hex("0x5aaeb6053f3e94c9b9a09f33669435e7ef1beaed");
    REQUIRE(addr.to_checksum() == "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed");
  }

  SECTION("Mixed case input") {
    // Should normalize to proper checksum
    Address addr =
        Address::from_hex("0x5AAEB6053f3E94C9b9a09f33669435e7ef1beaed");
    REQUIRE(addr.to_checksum() == "0x5aAeb6053F3E94C9b9A09f33669435E7Ef1BeAed");
  }

  SECTION("Zero address") {
    Address addr{};
    REQUIRE(addr.to_checksum() == "0x0000000000000000000000000000000000000000");
  }
}

TEST_CASE("secp256k1 signing", "[crypto][secp256k1]") {
  // Test with a known private key
  std::string_view test_key =
      "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80";

  auto key = PrivateKey::from_hex(test_key);
  REQUIRE(key.has_value());

  SECTION("Derive public key") {
    auto pubkey = key->public_key();
    REQUIRE(pubkey.has_value());
    REQUIRE(pubkey->size() == 65); // Uncompressed: 0x04 + 64 bytes
  }

  SECTION("Derive address") {
    auto addr = key->address();
    REQUIRE(addr.has_value());
    // This is the first Hardhat test account
    REQUIRE(addr->to_checksum() ==
            "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266");
  }

  SECTION("Sign message") {
    Keccak256Hash hash;
    std::fill(hash.begin(), hash.end(), 0x42); // Test message hash

    auto sig = key->sign(hash);
    REQUIRE(sig.has_value());
    REQUIRE((sig->v == 27 || sig->v == 28));
    REQUIRE(sig->r.size() == 32);
    REQUIRE(sig->s.size() == 32);
  }

  SECTION("Signature hex encoding") {
    Keccak256Hash hash;
    std::fill(hash.begin(), hash.end(), 0x00);

    auto sig = key->sign(hash);
    REQUIRE(sig.has_value());

    std::string hex = sig->to_hex();
    REQUIRE(hex.size() == 132); // "0x" + 65 bytes = 2 + 130 hex chars
  }
}

TEST_CASE("HMAC-SHA256", "[crypto][hmac]") {
  SECTION("Test vector") {
    // RFC 4231 test vector
    std::vector<uint8_t> key(20, 0x0b);
    std::string_view data = "Hi There";

    auto result = hmac_sha256(
        key,
        std::span{reinterpret_cast<const uint8_t *>(data.data()), data.size()});

    REQUIRE(result.has_value());
    REQUIRE(result->size() == 32);
  }

  SECTION("L2 signature generation") {
    std::string secret = "dGVzdHNlY3JldA=="; // "testsecret" in base64
    std::string timestamp = "1234567890";
    std::string method = "POST";
    std::string path = "/order";
    std::string body = R"({"test": "data"})";

    auto sig = generate_l2_signature(secret, timestamp, method, path, body);
    REQUIRE(sig.has_value());
    REQUIRE(!sig->empty());
  }
}

TEST_CASE("Base64 encoding", "[crypto][base64]") {
  SECTION("Encode") {
    std::vector<uint8_t> data = {'H', 'e', 'l', 'l', 'o'};
    std::string encoded = base64_encode(data);
    REQUIRE(encoded == "SGVsbG8=");
  }

  SECTION("Decode") {
    auto decoded = base64_decode("SGVsbG8=");
    REQUIRE(decoded.has_value());
    REQUIRE(decoded->size() == 5);
    REQUIRE((*decoded)[0] == 'H');
  }

  SECTION("URL-safe encode") {
    std::vector<uint8_t> data = {0xFF, 0xFE, 0xFD};
    std::string encoded = base64url_encode(data);
    // Should not contain + or /
    REQUIRE(encoded.find('+') == std::string::npos);
    REQUIRE(encoded.find('/') == std::string::npos);
  }

  SECTION("Roundtrip") {
    std::vector<uint8_t> original = {0x01, 0x23, 0x45, 0x67,
                                     0x89, 0xAB, 0xCD, 0xEF};
    std::string encoded = base64_encode(original);
    auto decoded = base64_decode(encoded);
    REQUIRE(decoded.has_value());
    REQUIRE(*decoded == original);
  }
}
