# Contributing

See [README.md](README.md) for dependencies and supported API scope.

```sh
make build test lint consumers
make format
```

Use C++23, tabs, and 100 columns as configured in .clang-format. Prefer
explicit local types except structured bindings, lambdas, and iterators.
Add behavioral regressions for fixes, especially numeric boundaries,
network input, callback lifetimes, signing, and installed consumers.
Default tests must use fixtures or loopback servers without credentials.

Submit a focused PR describing the problem, behavior, and validation.
Update CHANGELOG.md for user-visible changes. Linux, macOS, Windows, and
Markdown CI must pass before merge. Prepare releases through a PR, then
create a signed vX.Y.Z tag on verified main; release.yml publishes it.

Report vulnerabilities through [SECURITY.md](SECURITY.md). Contributions
are licensed under the repository's MIT license.
