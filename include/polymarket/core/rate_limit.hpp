#pragma once

/// @file rate_limit.hpp
/// @brief Token bucket rate limiter for API requests
///
/// Implements a thread-safe token bucket algorithm to prevent API rate limit
/// violations.

#include "polymarket/core/error.hpp"

#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>
#include <thread>

namespace polymarket {

/// Token bucket rate limiter
///
/// Implements a token bucket algorithm to limit request rates.
/// Thread-safe for concurrent access.
class RateLimiter {
public:
  /// Configuration for rate limiting
  struct Config {
    std::uint16_t max_tokens{10}; ///< Maximum tokens (0-65535)
    std::chrono::milliseconds refill_interval{1000}; ///< Time to add one token
    std::uint16_t initial_tokens{10};                ///< Starting tokens
    std::optional<std::chrono::milliseconds> max_wait{}; ///< Max time to wait
  };

  RateLimiter() : RateLimiter(Config{}) {}
  explicit RateLimiter(const Config &config)
      : config_(config), tokens_(config.initial_tokens),
        last_refill_(std::chrono::steady_clock::now()) {}

  /// Try to acquire a token, returns true if successful
  [[nodiscard]] bool try_acquire() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    refill();
    if (tokens_ > 0) {
      --tokens_;
      return true;
    }
    return false;
  }

  /// Acquire a token, blocking if necessary
  /// Returns false if max_wait exceeded
  [[nodiscard]] bool acquire() {
    if (!config_.max_wait) {
      // No max wait - keep trying
      while (!try_acquire()) {
        std::this_thread::sleep_for(config_.refill_interval / 10);
      }
      return true;
    }
    return acquire_for(*config_.max_wait);
  }

  /// Acquire a token, blocking up to max_wait
  [[nodiscard]] bool acquire_for(std::chrono::milliseconds max_wait) {
    auto deadline = std::chrono::steady_clock::now() + max_wait;
    while (!try_acquire()) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(config_.refill_interval / 10);
    }
    return true;
  }

  /// Get current number of available tokens
  [[nodiscard]] std::uint16_t available_tokens() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return tokens_;
  }

  /// Reset the rate limiter to initial state
  void reset() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    tokens_ = config_.initial_tokens;
    last_refill_ = std::chrono::steady_clock::now();
  }

  /// Get the configuration
  [[nodiscard]] const Config &config() const noexcept { return config_; }

private:
  void refill() noexcept {
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - last_refill_);

    if (elapsed >= config_.refill_interval) {
      auto tokens_to_add = static_cast<std::uint16_t>(
          elapsed.count() / config_.refill_interval.count());
      tokens_ = std::min(static_cast<std::uint16_t>(tokens_ + tokens_to_add),
                         config_.max_tokens);
      last_refill_ = now;
    }
  }

  Config config_;
  mutable std::mutex mutex_;
  std::uint16_t tokens_;
  std::chrono::steady_clock::time_point last_refill_;
};

/// Scoped rate limit acquisition
///
/// RAII wrapper that acquires a rate limit token on construction.
class ScopedRateLimit {
public:
  explicit ScopedRateLimit(RateLimiter &limiter)
      : acquired_(limiter.acquire()) {}

  /// Check if acquisition was successful
  [[nodiscard]] bool acquired() const noexcept { return acquired_; }

  /// Implicit conversion to bool for easy checking
  explicit operator bool() const noexcept { return acquired_; }

private:
  bool acquired_;
};

} // namespace polymarket
