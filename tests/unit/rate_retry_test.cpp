#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <thread>
#include <vector>

#include "polymarket/core/rate_limit.hpp"
#include "polymarket/core/retry.hpp"

TEST_CASE("Rate limiting bounds configuration, deadlines, and concurrent tokens", "[rate]") {
	polymarket::RateLimiter invalid(
		{.max_tokens = 0, .refill_interval = std::chrono::milliseconds{-1}, .initial_tokens = 100});
	REQUIRE(invalid.config().max_tokens == 1);
	REQUIRE(invalid.config().initial_tokens == 1);
	REQUIRE(invalid.config().refill_interval.count() == 1000);
	REQUIRE(invalid.acquire_for(std::chrono::milliseconds{0}));
	REQUIRE_FALSE(invalid.acquire_for(std::chrono::milliseconds{1}));
	polymarket::RateLimiter limiter(
		{.max_tokens = 16, .refill_interval = std::chrono::hours{1}, .initial_tokens = 16});
	std::atomic<int> count = 0;
	std::vector<std::thread> threads;
	for (int i = 0; i < 8; ++i)
		threads.emplace_back([&]() {
			for (int n = 0; n < 100; ++n)
				if (limiter.try_acquire())
					++count;
		});
	for (std::thread& thread : threads)
		thread.join();
	REQUIRE(count == 16);
	REQUIRE(limiter.available_tokens() == 0);
}
TEST_CASE("Retry delay stays finite and capped", "[retry]") {
	polymarket::RetryPolicy policy{.initial_delay = std::chrono::milliseconds{100},
								   .max_delay = std::chrono::milliseconds{100},
								   .jitter_factor = 1};
	for (int n = 0; n < 100; ++n)
		REQUIRE(polymarket::calculate_retry_delay(255, policy) <= policy.max_delay);
	policy.backoff_multiplier = std::numeric_limits<double>::infinity();
	REQUIRE(polymarket::calculate_retry_delay(255, policy) <= policy.max_delay);
	REQUIRE(polymarket::should_retry(polymarket::Error::timeout("fixture"), policy));
}
