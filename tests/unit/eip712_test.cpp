/// @file eip712_test.cpp
/// @brief Unit tests for EIP-712 typed data hashing

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "polymarket/crypto/eip712.hpp"
#include "polymarket/crypto/secp256k1.hpp"

using namespace polymarket;
using namespace polymarket::crypto;

TEST_CASE("EIP-712 Domain Separator", "[crypto][eip712]") {
  SECTION("CLOB Auth Domain") {
    auto domain = EIP712Domain::clob_auth(137); // Polygon mainnet
    REQUIRE(domain.name == "ClobAuthDomain");
    REQUIRE(domain.version == "1");
    REQUIRE(domain.chain_id == 137);
    REQUIRE(domain.verifying_contract == Address{}); // Zero address

    auto hash = domain.separator_hash();
    REQUIRE(hash.size() == 32);
  }

  SECTION("Exchange Domain") {
    auto domain = EIP712Domain::exchange(137);
    REQUIRE(domain.name == "Polymarket CTF Exchange");
    REQUIRE(domain.verifying_contract.to_checksum() ==
            "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E");

    auto hash = domain.separator_hash();
    REQUIRE(hash.size() == 32);
  }

  SECTION("NegRisk Exchange Domain") {
    auto domain = EIP712Domain::neg_risk_exchange(137);
    REQUIRE(domain.verifying_contract.to_checksum() ==
            "0xC5d563A36AE78145C45a50134d48A1215220f80a");
  }

  SECTION("Different chain IDs produce different separators") {
    auto polygon = EIP712Domain::exchange(137);
    auto mumbai = EIP712Domain::exchange(80001);

    auto hash1 = polygon.separator_hash();
    auto hash2 = mumbai.separator_hash();

    REQUIRE(hash1 != hash2);
  }
}

TEST_CASE("ClobAuth struct hashing", "[crypto][eip712]") {
  SECTION("Struct hash") {
    ClobAuth auth{
        .address =
            Address::from_hex("0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266"),
        .timestamp = "1234567890",
        .nonce = 0,
        .message = "This message attests that I control the given wallet"};

    auto hash = auth.struct_hash();
    REQUIRE(hash.size() == 32);
  }

  SECTION("Type hash includes domain") {
    ClobAuth auth{
        .address =
            Address::from_hex("0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266"),
        .timestamp = "1234567890",
        .nonce = 0,
        .message = "This message attests that I control the given wallet"};

    auto domain = EIP712Domain::clob_auth(137);
    auto type_hash = auth.type_hash(domain);

    // Type hash should be different from struct hash
    auto struct_hash = auth.struct_hash();
    REQUIRE(type_hash != struct_hash);
  }

  SECTION("Different timestamps produce different hashes") {
    ClobAuth auth1{
        .address =
            Address::from_hex("0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266"),
        .timestamp = "1234567890",
        .nonce = 0,
        .message = "This message attests that I control the given wallet"};

    ClobAuth auth2 = auth1;
    auth2.timestamp = "1234567891";

    REQUIRE(auth1.struct_hash() != auth2.struct_hash());
  }
}

TEST_CASE("Order struct hashing", "[crypto][eip712]") {
  SECTION("Basic order hash") {
    Order order{
        .salt = uint256_t::from_uint64(12345),
        .maker =
            Address::from_hex("0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266"),
        .signer =
            Address::from_hex("0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266"),
        .taker = Address{}, // Zero address = any taker
        .token_id = uint256_t::from_uint64(1),
        .maker_amount = uint256_t::from_uint64(1000000), // 1 USDC
        .taker_amount = uint256_t::from_uint64(1000000), // 1 share
        .expiration = uint256_t::from_uint64(1735689600),
        .nonce = uint256_t::from_uint64(0),
        .fee_rate_bps = uint256_t::from_uint64(0),
        .side = 0,          // BUY
        .signature_type = 0 // EOA
    };

    auto hash = order.struct_hash();
    REQUIRE(hash.size() == 32);
  }

  SECTION("Order type hash with neg_risk flag") {
    Order order{.salt = uint256_t::from_uint64(12345),
                .maker = Address::from_hex(
                    "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266"),
                .signer = Address::from_hex(
                    "0xf39Fd6e51aad88F6F4ce6aB8827279cffFb92266"),
                .taker = Address{},
                .token_id = uint256_t::from_uint64(1),
                .maker_amount = uint256_t::from_uint64(1000000),
                .taker_amount = uint256_t::from_uint64(1000000),
                .expiration = uint256_t::from_uint64(1735689600),
                .nonce = uint256_t::from_uint64(0),
                .fee_rate_bps = uint256_t::from_uint64(0),
                .side = 0,
                .signature_type = 0};

    // Standard vs NegRisk should produce different type hashes
    auto hash_standard = order.type_hash(false, 137);
    auto hash_neg_risk = order.type_hash(true, 137);

    REQUIRE(hash_standard != hash_neg_risk);
  }
}

TEST_CASE("EIP-712 signing flow", "[crypto][eip712]") {
  // Test with a known private key
  auto key = PrivateKey::from_hex(
      "0xac0974bec39a17e36ba4a6b4d238ff944bacb478cbed5efcae784d7bf4f2ff80");
  REQUIRE(key.has_value());

  auto address = key->address();
  REQUIRE(address.has_value());

  SECTION("Sign ClobAuth") {
    ClobAuth auth{.address = *address,
                  .timestamp = "1234567890",
                  .nonce = 0,
                  .message =
                      "This message attests that I control the given wallet"};

    auto sig = sign_clob_auth(*key, auth, 137);
    REQUIRE(sig.has_value());
    REQUIRE((sig->v == 27 || sig->v == 28));
  }

  SECTION("Sign Order") {
    Order order{.salt = uint256_t::from_uint64(12345),
                .maker = *address,
                .signer = *address,
                .taker = Address{},
                .token_id = uint256_t::from_uint64(1),
                .maker_amount = uint256_t::from_uint64(1000000),
                .taker_amount = uint256_t::from_uint64(1000000),
                .expiration = uint256_t::from_uint64(1735689600),
                .nonce = uint256_t::from_uint64(0),
                .fee_rate_bps = uint256_t::from_uint64(0),
                .side = 0,
                .signature_type = 0};

    auto sig = sign_order(*key, order, false, 137);
    REQUIRE(sig.has_value());
    REQUIRE((sig->v == 27 || sig->v == 28));

    // Second signature should also be valid
    // Note: ECDSA uses random nonces, so signatures won't be identical
    // unless deterministic (RFC 6979) signing is used
    auto sig2 = sign_order(*key, order, false, 137);
    REQUIRE(sig2.has_value());
    REQUIRE((sig2->v == 27 || sig2->v == 28));
    REQUIRE(sig->r.size() == sig2->r.size());
    REQUIRE(sig->s.size() == sig2->s.size());
  }
}

TEST_CASE("Type hash constants", "[crypto][eip712]") {
  SECTION("EIP712Domain type hash") {
    auto hash = type_hashes::eip712_domain();
    REQUIRE(hash.size() == 32);
    // Should be consistent
    auto hash2 = type_hashes::eip712_domain();
    REQUIRE(hash == hash2);
  }

  SECTION("ClobAuth type hash") {
    auto hash = type_hashes::clob_auth();
    REQUIRE(hash.size() == 32);
  }

  SECTION("Order type hash") {
    auto hash = type_hashes::order();
    REQUIRE(hash.size() == 32);
  }
}
