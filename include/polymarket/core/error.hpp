#pragma once

/// @file error.hpp
/// @brief Error handling types for the Polymarket C++ SDK
///
/// This module provides a comprehensive error system using C++23's
/// std::expected for zero-overhead error handling without exceptions.

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

namespace polymarket {

/// Error categories for the SDK
enum class ErrorCode : std::uint8_t {
	None = 0,		 ///< No error
	NetworkError,	 ///< Network/connection failure
	AuthError,		 ///< Authentication failure
	ValidationError, ///< Input validation failure
	RateLimitError,	 ///< Rate limit exceeded (HTTP 429)
	ServerError,	 ///< Server error (HTTP 5xx)
	ParseError,		 ///< JSON/response parsing failure
	CryptoError,	 ///< Cryptographic operation failure
	WebSocketError,	 ///< WebSocket-specific error
	TimeoutError,	 ///< Request timeout
	NotFoundError,	 ///< Resource not found (HTTP 404)
	BadRequestError, ///< Bad request (HTTP 400)
	InternalError,	 ///< Internal SDK error
};

/// Convert ErrorCode to string representation
[[nodiscard]] constexpr std::string_view to_string(ErrorCode code) noexcept {
	switch (code) {
		case ErrorCode::None:
			return "None";
		case ErrorCode::NetworkError:
			return "NetworkError";
		case ErrorCode::AuthError:
			return "AuthError";
		case ErrorCode::ValidationError:
			return "ValidationError";
		case ErrorCode::RateLimitError:
			return "RateLimitError";
		case ErrorCode::ServerError:
			return "ServerError";
		case ErrorCode::ParseError:
			return "ParseError";
		case ErrorCode::CryptoError:
			return "CryptoError";
		case ErrorCode::WebSocketError:
			return "WebSocketError";
		case ErrorCode::TimeoutError:
			return "TimeoutError";
		case ErrorCode::NotFoundError:
			return "NotFoundError";
		case ErrorCode::BadRequestError:
			return "BadRequestError";
		case ErrorCode::InternalError:
			return "InternalError";
	}
	return "Unknown";
}

/// Error class with code, message, and optional context
class Error {
public:
	/// Construct an error with code and message
	Error(ErrorCode code, std::string message) noexcept
		: code_(code), message_(std::move(message)) {}

	/// Construct an error with code, message, and HTTP status
	Error(ErrorCode code, std::string message, int http_status) noexcept
		: code_(code), message_(std::move(message)), http_status_(http_status) {}

	/// Get the error code
	[[nodiscard]] ErrorCode code() const noexcept { return code_; }

	/// Get the error message
	[[nodiscard]] const std::string& message() const noexcept { return message_; }

	/// Get the HTTP status code (if applicable)
	[[nodiscard]] std::optional<int> http_status() const noexcept { return http_status_; }

	/// Get a formatted error string
	[[nodiscard]] std::string to_string() const {
		std::string result = std::string(polymarket::to_string(code_)) + ": " + message_;
		if (http_status_) {
			result += " (HTTP " + std::to_string(*http_status_) + ")";
		}
		return result;
	}

	// Factory methods for common errors
	[[nodiscard]] static Error network(std::string msg) {
		return Error(ErrorCode::NetworkError, std::move(msg));
	}

	[[nodiscard]] static Error auth(std::string msg) {
		return Error(ErrorCode::AuthError, std::move(msg));
	}

	[[nodiscard]] static Error validation(std::string msg) {
		return Error(ErrorCode::ValidationError, std::move(msg));
	}

	[[nodiscard]] static Error rate_limit(std::string msg) {
		return Error(ErrorCode::RateLimitError, std::move(msg), 429);
	}

	[[nodiscard]] static Error server(std::string msg, int status = 500) {
		return Error(ErrorCode::ServerError, std::move(msg), status);
	}

	[[nodiscard]] static Error parse(std::string msg) {
		return Error(ErrorCode::ParseError, std::move(msg));
	}

	[[nodiscard]] static Error crypto(std::string msg) {
		return Error(ErrorCode::CryptoError, std::move(msg));
	}

	[[nodiscard]] static Error websocket(std::string msg) {
		return Error(ErrorCode::WebSocketError, std::move(msg));
	}

	[[nodiscard]] static Error timeout(std::string msg) {
		return Error(ErrorCode::TimeoutError, std::move(msg));
	}

	[[nodiscard]] static Error not_found(std::string msg) {
		return Error(ErrorCode::NotFoundError, std::move(msg), 404);
	}

	[[nodiscard]] static Error bad_request(std::string msg) {
		return Error(ErrorCode::BadRequestError, std::move(msg), 400);
	}

	[[nodiscard]] static Error internal(std::string msg) {
		return Error(ErrorCode::InternalError, std::move(msg));
	}

	/// Create error from HTTP status code
	[[nodiscard]] static Error from_http_status(int status, std::string msg) {
		ErrorCode code;
		switch (status) {
			case 400:
				code = ErrorCode::BadRequestError;
				break;
			case 401:
			case 403:
				code = ErrorCode::AuthError;
				break;
			case 404:
				code = ErrorCode::NotFoundError;
				break;
			case 429:
				code = ErrorCode::RateLimitError;
				break;
			default:
				if (status >= 500) {
					code = ErrorCode::ServerError;
				} else {
					code = ErrorCode::NetworkError;
				}
		}
		return Error(code, std::move(msg), status);
	}

private:
	ErrorCode code_;
	std::string message_;
	std::optional<int> http_status_;
};

/// Result type using C++23 std::expected
template <typename T>
using Result = std::expected<T, Error>;

/// Unit result type for operations that don't return a value
using VoidResult = std::expected<void, Error>;

} // namespace polymarket
