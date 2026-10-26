# Security

Security fixes target the latest published release. Upgrade your
FetchContent pin or installed polymarket package; older tags are not
back-patched.

Report SDK vulnerabilities through [GitHub private vulnerability reporting](https://github.com/Reddimus/polymarket-cpp/security/advisories/new).
Include the affected version/commit, reproduction, and impact. Do not
post credentials or exploit details publicly. Ordinary SDK bugs belong
in issues; service and dependency vulnerabilities go to their maintainers.

Signing helpers hold private keys in process memory. Keep credentials
outside source control and use HTTPS. Legacy CLOB code is retained for
migration and does not establish current trading compatibility.
