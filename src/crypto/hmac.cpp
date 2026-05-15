/// @file hmac.cpp
/// @brief HMAC-SHA256 and base64 encoding for L2 authentication

#include "polymarket/crypto/hmac.hpp"

#include <array>
#include <cstring>
#include <openssl/evp.h>
#include <openssl/hmac.h>

namespace polymarket::crypto {

namespace {

// Base64 encoding table (standard)
constexpr char BASE64_TABLE[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

// URL-safe base64 encoding table
constexpr char BASE64URL_TABLE[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string base64_encode_impl(std::span<const std::uint8_t> data, const char* table, bool pad) {
	std::string result;
	result.reserve((data.size() + 2) / 3 * 4);

	size_t i = 0;
	while (i + 2 < data.size()) {
		std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
						  (static_cast<std::uint32_t>(data[i + 1]) << 8) |
						  static_cast<std::uint32_t>(data[i + 2]);
		result += table[(n >> 18) & 0x3f];
		result += table[(n >> 12) & 0x3f];
		result += table[(n >> 6) & 0x3f];
		result += table[n & 0x3f];
		i += 3;
	}

	if (i + 1 == data.size()) {
		std::uint32_t n = static_cast<std::uint32_t>(data[i]) << 16;
		result += table[(n >> 18) & 0x3f];
		result += table[(n >> 12) & 0x3f];
		if (pad) {
			result += "==";
		}
	} else if (i + 2 == data.size()) {
		std::uint32_t n = (static_cast<std::uint32_t>(data[i]) << 16) |
						  (static_cast<std::uint32_t>(data[i + 1]) << 8);
		result += table[(n >> 18) & 0x3f];
		result += table[(n >> 12) & 0x3f];
		result += table[(n >> 6) & 0x3f];
		if (pad) {
			result += '=';
		}
	}

	return result;
}

Result<std::vector<std::uint8_t>> base64_decode_impl(std::string_view encoded, bool url_safe) {
	// Build decode table
	std::array<int, 256> decode_table;
	decode_table.fill(-1);

	const char* table = url_safe ? BASE64URL_TABLE : BASE64_TABLE;
	for (int i = 0; i < 64; ++i) {
		decode_table[static_cast<unsigned char>(table[i])] = i;
	}

	// Remove padding
	while (!encoded.empty() && encoded.back() == '=') {
		encoded = encoded.substr(0, encoded.size() - 1);
	}

	std::vector<std::uint8_t> result;
	result.reserve(encoded.size() * 3 / 4);

	std::uint32_t buffer = 0;
	int bits = 0;

	for (char c : encoded) {
		int value = decode_table[static_cast<unsigned char>(c)];
		if (value < 0) {
			// Skip whitespace
			if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
				continue;
			}
			return std::unexpected(Error::parse("Invalid base64 character"));
		}

		buffer = (buffer << 6) | value;
		bits += 6;

		if (bits >= 8) {
			bits -= 8;
			result.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xff));
		}
	}

	return result;
}

} // anonymous namespace

Result<HmacSha256Hash> hmac_sha256(std::span<const std::uint8_t> key,
								   std::span<const std::uint8_t> message) {
	HmacSha256Hash result;
	unsigned int len = 0;

	unsigned char* hmac = HMAC(EVP_sha256(), key.data(), static_cast<int>(key.size()),
							   message.data(), message.size(), result.data(), &len);

	if (!hmac || len != 32) {
		return std::unexpected(Error::crypto("HMAC-SHA256 failed"));
	}

	return result;
}

std::string base64url_encode(std::span<const std::uint8_t> data) {
	return base64_encode_impl(data, BASE64URL_TABLE, false);
}

Result<std::vector<std::uint8_t>> base64url_decode(std::string_view encoded) {
	return base64_decode_impl(encoded, true);
}

std::string base64_encode(std::span<const std::uint8_t> data) {
	return base64_encode_impl(data, BASE64_TABLE, true);
}

Result<std::vector<std::uint8_t>> base64_decode(std::string_view encoded) {
	return base64_decode_impl(encoded, false);
}

Result<std::string> generate_l2_signature(std::string_view api_secret, std::string_view timestamp,
										  std::string_view method, std::string_view path,
										  std::string_view body) {
	// Decode the base64 secret
	auto secret_bytes = base64_decode(api_secret);
	if (!secret_bytes) {
		return std::unexpected(secret_bytes.error());
	}

	// Build the message: timestamp + method + path + body
	std::string message;
	message.reserve(timestamp.size() + method.size() + path.size() + body.size());
	message += timestamp;
	message += method;
	message += path;
	message += body;

	// Compute HMAC
	auto hmac =
		hmac_sha256(*secret_bytes, std::span{reinterpret_cast<const std::uint8_t*>(message.data()),
											 message.size()});
	if (!hmac) {
		return std::unexpected(hmac.error());
	}

	// Return URL-safe base64 encoded signature
	return base64url_encode(*hmac);
}

} // namespace polymarket::crypto
