/// @file eip712.cpp
/// @brief EIP-712 typed data hashing and signing implementation

#include "polymarket/crypto/eip712.hpp"

#include <cstring>
#include <iomanip>
#include <sstream>

namespace polymarket::crypto {

namespace {

// Encode a uint256 for EIP-712 hashing (left-padded to 32 bytes)
void encode_uint256(const uint256_t& value, std::vector<std::uint8_t>& out) {
    out.insert(out.end(), value.bytes.begin(), value.bytes.end());
}

// Encode an address for EIP-712 hashing (left-padded to 32 bytes)
void encode_address(const Address& addr, std::vector<std::uint8_t>& out) {
    // Pad with 12 zero bytes
    for (int i = 0; i < 12; ++i) {
        out.push_back(0);
    }
    out.insert(out.end(), addr.bytes.begin(), addr.bytes.end());
}

// Encode a uint8 for EIP-712 hashing (left-padded to 32 bytes)
void encode_uint8(std::uint8_t value, std::vector<std::uint8_t>& out) {
    for (int i = 0; i < 31; ++i) {
        out.push_back(0);
    }
    out.push_back(value);
}

// Encode a uint64 for EIP-712 hashing (left-padded to 32 bytes)
void encode_uint64(std::uint64_t value, std::vector<std::uint8_t>& out) {
    for (int i = 0; i < 24; ++i) {
        out.push_back(0);
    }
    for (int i = 7; i >= 0; --i) {
        out.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xff));
    }
}

// Encode a string for EIP-712 hashing (keccak256 of the string)
Keccak256Hash encode_string(std::string_view str) {
    return keccak256(str);
}

} // anonymous namespace

// =========================================================================
// Type Hash Constants
// =========================================================================

namespace type_hashes {

const Keccak256Hash& eip712_domain() {
    static const Keccak256Hash hash = keccak256(
        "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)");
    return hash;
}

const Keccak256Hash& clob_auth() {
    static const Keccak256Hash hash = keccak256(
        "ClobAuth(address address,string timestamp,uint256 nonce,string message)");
    return hash;
}

const Keccak256Hash& order() {
    static const Keccak256Hash hash = keccak256(
        "Order(uint256 salt,address maker,address signer,address taker,uint256 tokenId,"
        "uint256 makerAmount,uint256 takerAmount,uint256 expiration,uint256 nonce,"
        "uint256 feeRateBps,uint8 side,uint8 signatureType)");
    return hash;
}

} // namespace type_hashes

// =========================================================================
// EIP712Domain Implementation
// =========================================================================

Keccak256Hash EIP712Domain::separator_hash() const {
    std::vector<std::uint8_t> encoded;
    encoded.reserve(32 * 5);  // type hash + 4 fields

    // Type hash
    const auto& type_hash = type_hashes::eip712_domain();
    encoded.insert(encoded.end(), type_hash.begin(), type_hash.end());

    // name (keccak256 of string)
    auto name_hash = encode_string(name);
    encoded.insert(encoded.end(), name_hash.begin(), name_hash.end());

    // version (keccak256 of string)
    auto version_hash = encode_string(version);
    encoded.insert(encoded.end(), version_hash.begin(), version_hash.end());

    // chainId (uint256)
    encode_uint64(chain_id, encoded);

    // verifyingContract (address, left-padded)
    encode_address(verifying_contract, encoded);

    return keccak256(encoded);
}

EIP712Domain EIP712Domain::clob_auth(std::uint64_t chain_id) {
    return EIP712Domain{
        .name = "ClobAuthDomain",
        .version = "1",
        .chain_id = chain_id,
        .verifying_contract = Address{}  // Zero address for off-chain auth
    };
}

EIP712Domain EIP712Domain::exchange(std::uint64_t chain_id) {
    return EIP712Domain{
        .name = "Polymarket CTF Exchange",
        .version = "1",
        .chain_id = chain_id,
        .verifying_contract = Address::from_hex(contracts::EXCHANGE)
    };
}

EIP712Domain EIP712Domain::neg_risk_exchange(std::uint64_t chain_id) {
    return EIP712Domain{
        .name = "Polymarket CTF Exchange",
        .version = "1",
        .chain_id = chain_id,
        .verifying_contract = Address::from_hex(contracts::NEG_RISK_EXCHANGE)
    };
}

// =========================================================================
// ClobAuth Implementation
// =========================================================================

Keccak256Hash ClobAuth::struct_hash() const {
    std::vector<std::uint8_t> encoded;
    encoded.reserve(32 * 5);  // type hash + 4 fields

    // Type hash
    const auto& type_hash = type_hashes::clob_auth();
    encoded.insert(encoded.end(), type_hash.begin(), type_hash.end());

    // address
    encode_address(address, encoded);

    // timestamp (keccak256 of string)
    auto ts_hash = encode_string(timestamp);
    encoded.insert(encoded.end(), ts_hash.begin(), ts_hash.end());

    // nonce (uint256)
    encode_uint64(nonce, encoded);

    // message (keccak256 of string)
    auto msg_hash = encode_string(message);
    encoded.insert(encoded.end(), msg_hash.begin(), msg_hash.end());

    return keccak256(encoded);
}

Keccak256Hash ClobAuth::type_hash(const EIP712Domain& domain) const {
    // EIP-712 hash: keccak256("\x19\x01" || domainSeparator || structHash)
    std::vector<std::uint8_t> encoded;
    encoded.reserve(2 + 32 + 32);

    encoded.push_back(0x19);
    encoded.push_back(0x01);

    auto domain_sep = domain.separator_hash();
    encoded.insert(encoded.end(), domain_sep.begin(), domain_sep.end());

    auto struct_h = struct_hash();
    encoded.insert(encoded.end(), struct_h.begin(), struct_h.end());

    return keccak256(encoded);
}

// =========================================================================
// Order Implementation
// =========================================================================

Keccak256Hash Order::struct_hash() const {
    std::vector<std::uint8_t> encoded;
    encoded.reserve(32 * 13);  // type hash + 12 fields

    // Type hash
    const auto& type_hash = type_hashes::order();
    encoded.insert(encoded.end(), type_hash.begin(), type_hash.end());

    // salt (uint256)
    encode_uint256(salt, encoded);

    // maker (address)
    encode_address(maker, encoded);

    // signer (address)
    encode_address(signer, encoded);

    // taker (address)
    encode_address(taker, encoded);

    // tokenId (uint256)
    encode_uint256(token_id, encoded);

    // makerAmount (uint256)
    encode_uint256(maker_amount, encoded);

    // takerAmount (uint256)
    encode_uint256(taker_amount, encoded);

    // expiration (uint256)
    encode_uint256(expiration, encoded);

    // nonce (uint256)
    encode_uint256(nonce, encoded);

    // feeRateBps (uint256)
    encode_uint256(fee_rate_bps, encoded);

    // side (uint8)
    encode_uint8(side, encoded);

    // signatureType (uint8)
    encode_uint8(signature_type, encoded);

    return keccak256(encoded);
}

Keccak256Hash Order::type_hash(bool neg_risk, std::uint64_t chain_id) const {
    // Select the appropriate domain based on neg_risk flag
    EIP712Domain domain = neg_risk 
        ? EIP712Domain::neg_risk_exchange(chain_id)
        : EIP712Domain::exchange(chain_id);

    // EIP-712 hash: keccak256("\x19\x01" || domainSeparator || structHash)
    std::vector<std::uint8_t> encoded;
    encoded.reserve(2 + 32 + 32);

    encoded.push_back(0x19);
    encoded.push_back(0x01);

    auto domain_sep = domain.separator_hash();
    encoded.insert(encoded.end(), domain_sep.begin(), domain_sep.end());

    auto struct_h = struct_hash();
    encoded.insert(encoded.end(), struct_h.begin(), struct_h.end());

    return keccak256(encoded);
}

// =========================================================================
// Signing Functions
// =========================================================================

Result<Signature> sign_clob_auth(
    const PrivateKey& key,
    const ClobAuth& auth,
    std::uint64_t chain_id) {
    
    EIP712Domain domain = EIP712Domain::clob_auth(chain_id);
    Keccak256Hash hash = auth.type_hash(domain);
    return key.sign(hash);
}

Result<Signature> sign_order(
    const PrivateKey& key,
    const Order& order,
    bool neg_risk,
    std::uint64_t chain_id) {
    
    Keccak256Hash hash = order.type_hash(neg_risk, chain_id);
    return key.sign(hash);
}

} // namespace polymarket::crypto
