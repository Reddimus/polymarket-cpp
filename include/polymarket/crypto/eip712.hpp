#pragma once

/// @file eip712.hpp
/// @brief EIP-712 Typed Structured Data Hashing and Signing
///
/// Implements the EIP-712 standard for signing human-readable structured data.
/// This is required for Polymarket order signing and authentication.
///
/// Reference: https://eips.ethereum.org/EIPS/eip-712

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "polymarket/core/error.hpp"
#include "polymarket/core/types.hpp"
#include "polymarket/crypto/keccak.hpp"
#include "polymarket/crypto/secp256k1.hpp"

namespace polymarket::crypto {

/// EIP-712 Domain Separator fields
struct EIP712Domain {
	std::string name;
	std::string version;
	std::uint64_t chain_id;
	Address verifying_contract;

	/// Compute the domain separator hash
	[[nodiscard]] Keccak256Hash separator_hash() const;

	/// Polymarket CLOB Auth domain (for L1 authentication)
	static EIP712Domain clob_auth(std::uint64_t chain_id = 137);

	/// Polymarket Exchange domain (for order signing)
	static EIP712Domain exchange(std::uint64_t chain_id = 137);

	/// Polymarket NegRisk Exchange domain (for neg-risk market orders)
	static EIP712Domain neg_risk_exchange(std::uint64_t chain_id = 137);
};

/// Contract addresses
namespace contracts {
/// Standard CTF Exchange on Polygon
inline constexpr std::string_view EXCHANGE = "0x4bFb41d5B3570DeFd03C39a9A4D8dE6Bd8B8982E";
/// NegRisk CTF Exchange on Polygon
inline constexpr std::string_view NEG_RISK_EXCHANGE = "0xC5d563A36AE78145C45a50134d48A1215220f80a";
/// Conditional Token Framework on Polygon
inline constexpr std::string_view CTF = "0x4D97DCd97eC945f40cF65F87097ACe5EA0476045";
/// USDC on Polygon
inline constexpr std::string_view USDC = "0x2791Bca1f2de4661ED88A30C99A7a9449Aa84174";
} // namespace contracts

/// ClobAuth struct for L1 authentication
struct ClobAuth {
	Address address;
	std::string timestamp;
	std::uint64_t nonce;
	std::string message{"This message attests that I control the given wallet"};

	/// Compute the struct hash
	[[nodiscard]] Keccak256Hash struct_hash() const;

	/// Compute the full EIP-712 hash (domain separator + struct hash)
	[[nodiscard]] Keccak256Hash type_hash(const EIP712Domain& domain) const;
};

/// Order struct for CLOB order signing
struct Order {
	uint256_t salt;
	Address maker;
	Address signer;
	Address taker;
	uint256_t token_id;
	uint256_t maker_amount;
	uint256_t taker_amount;
	uint256_t expiration;
	uint256_t nonce;
	uint256_t fee_rate_bps;
	std::uint8_t side;			 // 0 = BUY, 1 = SELL
	std::uint8_t signature_type; // 0 = EOA, 1 = Proxy, 2 = GnosisSafe

	/// Compute the struct hash
	[[nodiscard]] Keccak256Hash struct_hash() const;

	/// Compute the full EIP-712 hash for signing
	/// @param neg_risk If true, uses NegRisk Exchange domain
	[[nodiscard]] Keccak256Hash type_hash(bool neg_risk, std::uint64_t chain_id = 137) const;
};

/// Order side enum
enum class Side : std::uint8_t {
	Buy = 0,
	Sell = 1,
};

/// Signature type enum
enum class SignatureType : std::uint8_t {
	EOA = 0,	   ///< Direct wallet signature
	Proxy = 1,	   ///< Magic Link / Proxy wallet
	GnosisSafe = 2 ///< Gnosis Safe multisig
};

/// Order type enum
enum class OrderType {
	GTC, ///< Good 'til Cancelled
	FOK, ///< Fill or Kill
	GTD, ///< Good 'til Date
	FAK, ///< Fill and Kill
};

/// Convert OrderType to string
[[nodiscard]] inline std::string_view to_string(OrderType type) {
	switch (type) {
		case OrderType::GTC:
			return "GTC";
		case OrderType::FOK:
			return "FOK";
		case OrderType::GTD:
			return "GTD";
		case OrderType::FAK:
			return "FAK";
	}
	return "GTC";
}

/// Sign a ClobAuth struct for L1 authentication
[[nodiscard]] Result<Signature> sign_clob_auth(const PrivateKey& key, const ClobAuth& auth,
											   std::uint64_t chain_id = 137);

/// Sign an Order struct for CLOB order placement
[[nodiscard]] Result<Signature> sign_order(const PrivateKey& key, const Order& order, bool neg_risk,
										   std::uint64_t chain_id = 137);

/// Type hash constants (precomputed)
namespace type_hashes {
/// keccak256("EIP712Domain(string name,string version,uint256 chainId,address
/// verifyingContract)")
[[nodiscard]] const Keccak256Hash& eip712_domain();

/// keccak256("ClobAuth(address address,string timestamp,uint256 nonce,string
/// message)")
[[nodiscard]] const Keccak256Hash& clob_auth();

/// keccak256("Order(uint256 salt,address maker,address signer,address
/// taker,uint256 tokenId,uint256 makerAmount,uint256 takerAmount,uint256
/// expiration,uint256 nonce,uint256 feeRateBps,uint8 side,uint8
/// signatureType)")
[[nodiscard]] const Keccak256Hash& order();
} // namespace type_hashes

} // namespace polymarket::crypto
