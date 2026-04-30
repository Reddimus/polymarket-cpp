# Changelog

All notable changes to **polymarket-cpp** are recorded here. The format
follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and
the project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### CI

- First-ever CI workflow added — build + test + lint on Ubuntu 24.04,
  build-only on macos-latest (test step disabled pending investigation
  of crypto-test failures).
- `.markdownlint-cli2.yaml` config disables `MD013` and other style-
  noise rules; enforces blank-line discipline.
- `.github/workflows/release.yml` auto-creates a GitHub Release when
  a `vX.Y.Z` tag is pushed (body sourced from this CHANGELOG).

### Known issues

- macOS `Test` step is disabled (`if: false`) pending root-cause of:
  - Test #1 Keccak-256 test vectors (hash output differs from Linux)
  - Test #3 EIP-55 checksum addresses
  - Test #4 secp256k1 signing
  - Test #7 EIP-712 Domain Separator

  Likely upstream OpenSSL version drift between brew (HEAD LLVM
  + OpenSSL 3.5+) and apt (OpenSSL 3.0). The Build step still verifies
  macOS portability of the compile path.

### Initial scope

This project has not yet cut a versioned release. Current capabilities:

- **CLOB API**: order builders, EIP-712 signing flow, balance queries,
  ClobAuth + Order struct hashing, types layer (`uint256_t`, `Address`,
  `Decimal`, `Bytes32`)
- **Crypto**: Keccak-256, secp256k1, EIP-712, HMAC-SHA256, Ed25519
- **HTTP**: libcurl wrapper with retry + timeout configuration

Stubs (TODO: implement):

- Gamma API client
- Data API client
- US (US-restricted markets) client
- WebSocket subscriptions / order-history streaming
- ECDSA public-key recovery in secp256k1 (signing works; recovery TODO)

[Unreleased]: https://github.com/Reddimus/polymarket-cpp/commits/main
