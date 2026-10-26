# Order building

US orders use us::OrderRequest and us::Client. Supported fields are
listed in [client.hpp](../include/polymarket/us/client.hpp). Methods return
Result; check it before using a response.

The clob module's Limit and Market builders produce legacy SignableOrder
objects. Supply the required token ID, side, and positive size/amount;
limit orders also require a price. build() validates required fields,
prices, and checked amount arithmetic, returning errors through Result.
This is a legacy wire contract, not current CLOB V2 support.

Use decimal strings for prices and sizes. Decimal retains six fractional
places, truncating extra digits. Malformed values and overflow throw;
unsigned scaling rejects overflow and clamps negative values to zero.
uint256_t retains full-width token IDs. Never pass a token ID through a
floating-point number.

For current order fields and supported workflows, consult the separate
[US](https://docs.polymarket.us/concepts/orders.md) and
[international](https://docs.polymarket.com/trading/place-orders.md) docs.
