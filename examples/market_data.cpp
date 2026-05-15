/// @file market_data.cpp
/// @brief Example: Fetching public market data from Polymarket
///
/// @warning **CLOB V1 deprecated 2026-04-28.** Polymarket upgraded
/// `clob.polymarket.com` to V2 at the 2026-04-28 cutover and V1 is "no
/// longer supported". Read paths (GET /markets, etc.) may still respond
/// briefly during the transition but cannot be relied on. See
/// `include/polymarket/clob/client.hpp` and `CHANGELOG.md`. For
/// production market-data on the regulated US venue, see
/// `examples/us_smoke.cpp`.

#include <iostream>
#include <polymarket/clob/client.hpp>

int main() {
	using namespace polymarket;
	using namespace polymarket::clob;

	// Create client
	auto client_result = Client::create();
	if (!client_result) {
		std::cerr << "Failed to create client: " << client_result.error().message() << "\n";
		return 1;
	}
	auto& client = *client_result;

	// Health check
	auto health = client.ok();
	if (!health) {
		std::cerr << "Health check failed: " << health.error().message() << "\n";
		return 1;
	}
	std::cout << "Server status: " << health->status << "\n";

	// Get server time
	auto time = client.server_time();
	if (time) {
		std::cout << "Server timestamp: " << time->timestamp << "\n";
	}

	// Get tick size for a token (example token ID)
	uint256_t token_id =
		uint256_t::from_hex("0x123456789abcdef123456789abcdef123456789abcdef123456789abcdef1234");

	auto tick_size_result = client.tick_size(token_id);
	if (tick_size_result) {
		std::cout << "Tick size fetched successfully\n";
	}

	// Check geoblock
	auto geoblock = client.check_geoblock();
	if (geoblock) {
		std::cout << "Geoblock status - Blocked: " << (geoblock->blocked ? "yes" : "no")
				  << ", Country: " << geoblock->country_code << "\n";
	}

	std::cout << "Market data example complete.\n";
	return 0;
}
