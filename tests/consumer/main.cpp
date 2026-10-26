#include <iostream>
#include <polymarket/core/types.hpp>
#include <polymarket/crypto/secp256k1.hpp>
#include <polymarket/gamma/client.hpp>
#include <polymarket/us/client.hpp>
#include <polymarket/version.hpp>
#include <string_view>
int main(int argc, char**) {
	if (std::string_view(polymarket::version()) != "0.5.0")
		return 1;
	const std::string maximum =
		"115792089237316195423570985008687907853269984665640564039457584007913129639935";
	if (polymarket::uint256_t::from_string(maximum).to_string() != maximum)
		return 2;
	const auto key = polymarket::crypto::PrivateKey::from_hex(std::string(63, '0') + "1");
	if (!key)
		return 5;
	const polymarket::crypto::Keccak256Hash hash{};
	const auto signature = key->sign(hash);
	const auto address = key->address();
	if (!signature || !address)
		return 6;
	const auto recovered = polymarket::crypto::recover_address(hash, *signature);
	if (!recovered || *recovered != *address)
		return 7;
	polymarket::gamma::Client gamma;
	if (gamma.get_tags())
		return 3;
	polymarket::us::Client client;
	if (argc > 1) {
		const polymarket::Result<std::string> markets = client.get_markets({.limit = 1});
		if (!markets) {
			std::cerr << markets.error().message() << '\n';
			return 4;
		}
		std::cout << "Public US markets read: " << markets->size() << " bytes\n";
	}
	return 0;
}
