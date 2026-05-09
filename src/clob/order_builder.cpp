/// @file order_builder.cpp
/// @brief Type-safe order builder implementation

#include "polymarket/clob/order_builder.hpp"
#include "polymarket/crypto/eip712.hpp"

#include <chrono>
#include <cstdint>
#include <random>

#if defined(_MSC_VER) && !defined(__clang__)
#include <intrin.h>
#endif

namespace polymarket::clob {

namespace {

/// Compute ``(a * b) / divisor`` without overflowing the 64-bit product.
///
/// The pattern (shares * price_scaled) / SCALE shows up in every
/// limit/market-order amount calculation. For typical retail orders
/// the product fits in 64 bits, but Polymarket allows institutional
/// sizes where ``shares`` can exceed 10^10 — at that point
/// ``shares * price_scaled`` overflows ``uint64_t``. GCC/Clang
/// provide ``__uint128_t`` as a built-in extension; MSVC does not,
/// so the Windows path uses the ``_umul128`` / ``_udiv128`` x86-64
/// intrinsics for a 128-bit-precise multiply-then-divide.
inline std::uint64_t mul_div_u64(std::uint64_t a, std::uint64_t b,
                                 std::uint64_t divisor) {
#if defined(__SIZEOF_INT128__)
  return static_cast<std::uint64_t>(
      (static_cast<unsigned __int128>(a) * b) / divisor);
#elif defined(_MSC_VER) && !defined(__clang__)
  std::uint64_t hi;
  std::uint64_t lo = _umul128(a, b, &hi);
  if (hi == 0) {
    return lo / divisor;
  }
  std::uint64_t rem;
  return _udiv128(hi, lo, divisor, &rem);
#else
#error "mul_div_u64 needs __uint128_t (GCC/Clang) or _umul128 (MSVC x64)"
#endif
}

/// Calculate amounts for limit order
/// For CLOB:
/// - BUY: spending USDC (makerAmount) to get shares (takerAmount)
///   makerAmount = size * price
///   takerAmount = size
/// - SELL: spending shares (makerAmount) to get USDC (takerAmount)
///   makerAmount = size
///   takerAmount = size * price
std::pair<uint256_t, uint256_t>
calculate_limit_amounts(Side side, const Decimal &price, const Decimal &size) {

  constexpr uint64_t SCALE = 1'000'000; // 10^6 for USDC decimals

  // Size is in shares (10^6 precision)
  uint64_t shares = size.to_uint64_scaled(6);
  // Price is between 0.001 and 0.999
  uint64_t price_scaled = price.to_uint64_scaled(6); // price * 10^6

  // USDC amount = shares * price / 10^6
  uint64_t usdc = mul_div_u64(shares, price_scaled, SCALE);

  if (side == Side::Buy) {
    // Maker gives USDC, taker gives shares
    return {uint256_t::from_uint64(usdc), uint256_t::from_uint64(shares)};
  } else {
    // Maker gives shares, taker gives USDC
    return {uint256_t::from_uint64(shares), uint256_t::from_uint64(usdc)};
  }
}

/// Get default expiration (1 day from now)
uint256_t default_expiration() {
  auto now = std::chrono::system_clock::now();
  auto future = now + std::chrono::hours(24);
  auto timestamp = std::chrono::duration_cast<std::chrono::seconds>(
                       future.time_since_epoch())
                       .count();
  return uint256_t::from_uint64(static_cast<uint64_t>(timestamp));
}

} // anonymous namespace

// =========================================================================
// LimitOrderBuilder Implementation
// =========================================================================

Result<SignableOrder> OrderBuilder<Limit>::build() const {
  // Validate required fields
  if (!token_id_) {
    return std::unexpected(Error::validation("token_id is required"));
  }
  if (!side_) {
    return std::unexpected(Error::validation("side is required"));
  }
  if (!price_) {
    return std::unexpected(
        Error::validation("price is required for limit orders"));
  }
  if (!size_) {
    return std::unexpected(
        Error::validation("size is required for limit orders"));
  }

  // Validate price bounds
  if (*price_ < Decimal::from_double(0.001) ||
      *price_ > Decimal::from_double(0.999)) {
    return std::unexpected(
        Error::validation("price must be between 0.001 and 0.999"));
  }

  // Calculate amounts
  auto [maker_amount, taker_amount] =
      calculate_limit_amounts(*side_, *price_, *size_);

  // Generate salt
  uint64_t salt = generate_salt();

  // Create the signable order
  SignableOrder signable;
  signable.order.salt = uint256_t::from_uint64(salt);
  signable.order.maker = maker_;
  signable.order.signer = signer_;
  signable.order.taker = taker_.value_or(Address{}); // Zero address = any taker
  signable.order.token_id = *token_id_;
  signable.order.maker_amount = maker_amount;
  signable.order.taker_amount = taker_amount;
  signable.order.expiration =
      expiration_.value_or(0) > 0
          ? uint256_t::from_uint64(expiration_.value_or(0))
          : default_expiration();
  signable.order.nonce = uint256_t::from_uint64(nonce_.value_or(0));
  signable.order.fee_rate_bps = uint256_t::from_uint64(fee_rate_);
  signable.order.side = static_cast<uint8_t>(*side_);
  signable.order.signature_type = static_cast<uint8_t>(signature_type_);
  signable.order_type = order_type_.value_or(OrderType::GTC);
  signable.post_only = post_only_;

  return signable;
}

// =========================================================================
// MarketOrderBuilder Implementation
// =========================================================================

Result<SignableOrder> OrderBuilder<Market>::build() const {
  // Validate required fields
  if (!token_id_) {
    return std::unexpected(Error::validation("token_id is required"));
  }
  if (!side_) {
    return std::unexpected(Error::validation("side is required"));
  }
  if (!amount_) {
    return std::unexpected(
        Error::validation("amount is required for market orders"));
  }

  // Market orders need a price for calculation
  Decimal effective_price =
      price_.value_or((*side_ == Side::Buy) ? Decimal::from_double(0.999)
                                            : Decimal::from_double(0.001));

  // Calculate amounts based on amount type
  uint256_t maker_amount, taker_amount;
  constexpr uint64_t SCALE = 1'000'000;

  uint64_t amount_scaled = amount_->to_uint64_scaled(6);
  uint64_t price_scaled = effective_price.to_uint64_scaled(6);

  if (is_usdc_) {
    // Amount is in USDC
    uint64_t usdc = amount_scaled;
    // shares = usdc * SCALE / price_scaled
    uint64_t shares = mul_div_u64(usdc, SCALE, price_scaled);

    if (*side_ == Side::Buy) {
      maker_amount = uint256_t::from_uint64(usdc);
      taker_amount = uint256_t::from_uint64(shares);
    } else {
      maker_amount = uint256_t::from_uint64(shares);
      taker_amount = uint256_t::from_uint64(usdc);
    }
  } else {
    // Amount is in shares
    uint64_t shares = amount_scaled;
    uint64_t usdc = mul_div_u64(shares, price_scaled, SCALE);

    if (*side_ == Side::Buy) {
      maker_amount = uint256_t::from_uint64(usdc);
      taker_amount = uint256_t::from_uint64(shares);
    } else {
      maker_amount = uint256_t::from_uint64(shares);
      taker_amount = uint256_t::from_uint64(usdc);
    }
  }

  // Generate salt
  uint64_t salt = generate_salt();

  // Create the signable order
  SignableOrder signable;
  signable.order.salt = uint256_t::from_uint64(salt);
  signable.order.maker = maker_;
  signable.order.signer = signer_;
  signable.order.taker = taker_.value_or(Address{});
  signable.order.token_id = *token_id_;
  signable.order.maker_amount = maker_amount;
  signable.order.taker_amount = taker_amount;
  signable.order.expiration =
      default_expiration(); // Short expiration for market orders
  signable.order.nonce = uint256_t::from_uint64(nonce_.value_or(0));
  signable.order.fee_rate_bps = uint256_t::from_uint64(fee_rate_);
  signable.order.side = static_cast<uint8_t>(*side_);
  signable.order.signature_type = static_cast<uint8_t>(signature_type_);
  signable.order_type = order_type_.value_or(OrderType::FOK);

  return signable;
}

} // namespace polymarket::clob
