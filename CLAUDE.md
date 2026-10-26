# Development guide

Read README.md for supported scope, dependencies, and runtime policies.
Run make build test lint consumers before submitting. CI tests Linux,
macOS, and Windows. Use C++23, tabs, 100 columns, and explicit local types
except iterators, structured bindings, and lambdas.

Static modules separate core types/errors, HTTP, crypto, US, and legacy
CLOB. The hash primitive sits below core/crypto to avoid static archive
cycles. Glaze is private; common checked numeric/JSON helpers are in
src/detail. Every public Gamma/Data method must link and fail explicitly
until implemented. Do not claim complete platform or CLOB V2 coverage.
Keep deterministic full-width ID, decimal overflow, HTTP bounds, signing,
WebSocket lifetime, and consumer integration regressions. No live orders
belong in default tests. Keep comments and documentation concise.
