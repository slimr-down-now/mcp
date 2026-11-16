# Changelog

## Unreleased

- Replace `libhv` and `slimr-curl` with `cpp-httplib[openssl]` (`httplib::Server`).
- Internal: switch macOS builds to llvm@23 with static libc++.
- Internal: harden CLI rdbuf guard and InvalidRequest test assertion.
- Internal: raise slimr-mcp line coverage to 91.18%.
- Internal: align tooling with the cpp-templates library template.
- Split `slimr-mcp` from `slimr-cpp` into its own vcpkg registry repository.
- Internal: move the repository to `slimr-down-now/mcp`
