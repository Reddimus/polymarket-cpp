# Authentication

Polymarket US and international CLOB authentication are separate.

For implemented US methods, construct us::Client and call
set_credentials with the platform key ID and base64 secret. The SDK
validates a 64-byte decoded secret and uses its first 32 bytes as the
Ed25519 seed. Requests send X-PM-Access-Key, X-PM-Timestamp, and
X-PM-Signature headers; signing uses timestamp + method + path.
See the [current US reference](https://docs.polymarket.us/) for key
provisioning and endpoint requirements.

The polymarket-us-cli accepts --credentials with a JSON file containing
key_id and secret_key. Restrict access to that file. It reads one JSON
command from stdin and writes a structured result to stdout; supported commands are get_account, get_balance, get_order, get_orders,
get_positions, place_order, and cancel_order. Avoid embedding secrets in shell commands
or committed .env files.

The legacy clob module contains EIP-712 wallet signing and API-key HMAC
helpers. Its wire contract predates the current international CLOB
migration. See [current international authentication](https://docs.polymarket.com/trading/wallets-auth.md)
before adapting it; these helpers are not a complete current integration.
