#pragma once

/// @file retry.hpp
/// @brief Retry policy with exponential backoff and jitter
///
/// Provides configurable retry logic for transient failures.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <functional>
#include <random>
#include <thread>

#include "polymarket/core/error.hpp"

namespace polymarket {

/// HTTP response for retry logic
struct HttpResponse {
	int status_code{0};
	std::string body;
	std::string content_type;
};

/// Retry policy configuration
/// Fields ordered for minimal padding; narrowed types reduce cache footprint.
struct RetryPolicy {
	std::chrono::milliseconds initial_delay{100};
	std::chrono::milliseconds max_delay{10000};
	double backoff_multiplier{2.0};
	double jitter_factor{0.1};	  ///< Random jitter as fraction of delay
	std::uint8_t max_attempts{3}; ///< max 255 attempts
	bool retry_on_network_error{true};
	bool retry_on_rate_limit{true};
	bool retry_on_server_error{true}; ///< 5xx errors
};

/// Result of a retry operation
struct RetryResult {
	std::chrono::milliseconds total_delay{0};
	std::uint8_t attempts_made{0};
	bool succeeded{false};
};

/// Determines if a response should be retried
[[nodiscard]] inline bool should_retry(const HttpResponse& response,
									   const RetryPolicy& policy) noexcept {
	// Rate limited (429)
	if (policy.retry_on_rate_limit && response.status_code == 429) {
		return true;
	}
	// Server errors (5xx)
	if (policy.retry_on_server_error && response.status_code >= 500 && response.status_code < 600) {
		return true;
	}
	return false;
}

/// Determines if an error should be retried
[[nodiscard]] inline bool should_retry(const Error& error, const RetryPolicy& policy) noexcept {
	if (policy.retry_on_network_error &&
		(error.code() == ErrorCode::NetworkError || error.code() == ErrorCode::TimeoutError)) {
		return true;
	}
	if (policy.retry_on_rate_limit && error.code() == ErrorCode::RateLimitError) {
		return true;
	}
	if (policy.retry_on_server_error && error.code() == ErrorCode::ServerError) {
		return true;
	}
	return false;
}

/// Calculate delay for a retry attempt with exponential backoff and jitter
[[nodiscard]] inline std::chrono::milliseconds calculate_retry_delay(std::uint8_t attempt,
																	 const RetryPolicy& policy) {
	const std::chrono::milliseconds maximum =
		std::max(policy.max_delay, std::chrono::milliseconds::zero());
	const double max_ms = static_cast<double>(maximum.count());
	double delay_ms = static_cast<double>(
		std::clamp(policy.initial_delay, std::chrono::milliseconds::zero(), maximum).count());
	const double multiplier =
		std::isfinite(policy.backoff_multiplier) ? std::max(policy.backoff_multiplier, 0.0) : 1.0;
	for (std::uint8_t i = 1; i < attempt; ++i) {
		delay_ms = std::min(delay_ms * multiplier, max_ms);
	}
	const double spread =
		std::isfinite(policy.jitter_factor) ? std::clamp(policy.jitter_factor, 0.0, 1.0) : 0.0;
	if (spread > 0.0) {
		static thread_local std::mt19937 rng{std::random_device{}()};
		std::uniform_real_distribution<double> jitter(1.0 - spread, 1.0 + spread);
		delay_ms *= jitter(rng);
	}
	// Comparing before conversion also handles overflow and rounded INT64_MAX.
	if (!std::isfinite(delay_ms) || delay_ms >= max_ms) {
		return maximum;
	}
	return std::chrono::milliseconds{static_cast<std::int64_t>(delay_ms)};
}

/// Execute an operation with retry logic
///
/// @param operation Function that performs the operation and returns
/// Result<HttpResponse>
/// @param policy Retry configuration
/// @return Result of the operation after retries
template <typename Operation>
[[nodiscard]] Result<HttpResponse> with_retry(Operation&& operation, const RetryPolicy& policy) {
	for (std::uint8_t attempt = 1; attempt <= policy.max_attempts; ++attempt) {
		Result<HttpResponse> result = operation();

		if (result.has_value()) {
			// Check if we should retry based on response
			if (should_retry(*result, policy) && attempt < policy.max_attempts) {
				std::chrono::milliseconds delay = calculate_retry_delay(attempt, policy);
				std::this_thread::sleep_for(delay);
				continue;
			}
			return result;
		}

		// Check if we should retry based on error
		if (should_retry(result.error(), policy) && attempt < policy.max_attempts) {
			std::chrono::milliseconds delay = calculate_retry_delay(attempt, policy);
			std::this_thread::sleep_for(delay);
			continue;
		}

		return result;
	}

	return std::unexpected(Error::network("Max retry attempts exceeded"));
}

} // namespace polymarket
