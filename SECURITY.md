# Security Policy

`polymarket-cpp` is a third-party C++ client for the offshore Polymarket
CLOB plus the CFTC-regulated Polymarket US gateway. It performs Ed25519
+ secp256k1 + EIP-712 signing and holds private keys in process memory,
so a vulnerability in the crypto path or in credential handling could
put live trading capital at risk. This file is the canonical contact
path for reporting one.

## Supported Versions

Security fixes are made on the latest published `vX.Y.Z` tag. Older
tags are not back-patched — bump your `FetchContent_Declare(... GIT_TAG ...)`
pin or your `find_package(polymarket-cpp X.Y.Z REQUIRED)` constraint to
the latest minor on the same major as part of the upgrade.

| Version    | Supported          |
| ---------- | ------------------ |
| latest tag | :white_check_mark: |
| older      | :x:                |

## Reporting a Vulnerability

**Do not open a public issue.** Use GitHub's [private vulnerability
reporting](https://github.com/Reddimus/polymarket-cpp/security/advisories/new)
flow, which delivers the report to the maintainer privately and
tracks coordinated disclosure.

When reporting, please include:

- Affected version (tag or commit SHA)
- A reproduction — minimal code or test case
- Impact (credential leak / signature forgery / wrong-amount transfer /
  DoS / something else)
- Whether you've notified anyone else (e.g. Polymarket directly)

You can expect:

- Acknowledgement within **3 business days**
- An initial assessment + severity rating within **7 business days**
- A fix on a new `vX.Y.Z+1` tag, or a clear timeline if the fix is
  larger

## Out of Scope

- Bugs against `polymarket.com` / `polymarket.us` themselves — those go
  to Polymarket's own vulnerability program, not this client library.
- Operational issues (rate-limit handling, network blips) — file a
  regular issue.
- Theoretical issues against dependencies — report them upstream
  (`openssl`, `libcurl`, `cpp-httplib`, `libwebsockets`,
  `libsecp256k1`, `rapidjson`, `nlohmann/json`, `catch2`). We pin
  via FetchContent and bump on credible advisories.
