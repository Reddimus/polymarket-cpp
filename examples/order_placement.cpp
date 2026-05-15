/// @file order_placement.cpp
/// @brief Example: Placing orders on Polymarket (requires credentials)
///
/// @warning **CLOB V1 deprecated 2026-04-28.** This example uses the legacy
/// V1 order shape (`nonce` / `feeRateBps` / `taker`); Polymarket upgraded
/// `clob.polymarket.com` to V2 at the 2026-04-28 cutover and V1 is "no
/// longer supported". Running this example against the live endpoint will
/// fail. See `include/polymarket/clob/client.hpp` and `CHANGELOG.md` for
/// context. For live trading prefer `polymarket::us::Client` (CFTC-
/// regulated, Ed25519, production-ready) — see `examples/us_smoke.cpp`.

#include <cstdlib>
#include <iostream>
#include <polymarket/clob/client.hpp>
#include <polymarket/crypto/secp256k1.hpp>

int main() {
	using namespace polymarket;
	using namespace polymarket::clob;
	using namespace polymarket::crypto;

	// Get credentials from environment
	const char* api_key = std::getenv("POLYMARKET_API_KEY");
	const char* api_secret = std::getenv("POLYMARKET_API_SECRET");
	const char* api_passphrase = std::getenv("POLYMARKET_PASSPHRASE");
	const char* signer_address = std::getenv("POLYMARKET_ADDRESS");

	if (!api_key || !api_secret || !api_passphrase || !signer_address) {
		std::cerr << "Set environment variables: POLYMARKET_API_KEY, "
					 "POLYMARKET_API_SECRET, "
				  << "POLYMARKET_PASSPHRASE, POLYMARKET_ADDRESS\n";
		return 1;
	}

	// Create client
	auto client_result = Client::create();
	if (!client_result) {
		std::cerr << "Failed to create client\n";
		return 1;
	}
	auto& client = *client_result;

	// Authenticate with API credentials
	ApiKeyCredentials creds;
	creds.api_key = api_key;
	creds.api_secret = api_secret;
	creds.api_passphrase = api_passphrase;

	Address signer = Address::from_hex(signer_address);
	auto auth = client.authenticate(creds, signer);
	if (!auth) {
		std::cerr << "Authentication failed: " << auth.error().message() << "\n";
		return 1;
	}

	std::cout << "Authenticated successfully\n";

	// Example token ID
	uint256_t token_id =
		uint256_t::from_hex("0x123456789abcdef123456789abcdef123456789abcdef123456789abcdef1234");

	// Create a limit order builder
	auto builder_result = client.limit_order(token_id);
	if (!builder_result) {
		std::cerr << "Failed to create order builder: " << builder_result.error().message() << "\n";
		return 1;
	}

	// Configure the order
	LimitOrderBuilder& builder = *builder_result;
	builder.token_id(token_id);
	builder.side(Side::Buy);
	builder.price(Decimal::from_double(0.50)); // 50 cents
	builder.size(Decimal::from_double(10.0));  // 10 shares

	// Build the order
	auto order = builder.build();
	if (!order) {
		std::cerr << "Failed to build order: " << order.error().message() << "\n";
		return 1;
	}

	std::cout << "Order built successfully\n";
	std::cout << "Note: To sign and post, you would need a private key.\n";
	std::cout << "Example complete.\n";

	return 0;
}
