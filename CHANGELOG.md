# Changelog

## Unreleased

- Refactor `slimr-mcp` as an HTTP-only MCP `2026-07-28` adapter on `slimr-serve` (`structuredContent`/`outputSchema` for typed outputs, `apiVersion` in `server/discover`, Unix socket OAuth bypass) and remove stdio transport (`<bin> mcp`).
- Docs: fix HTTP socket write timeout in README (500 ms).
- Internal: pin slimr-serve registry baseline and drop overlay-ports in vcpkg-configuration.json.

## v0.1.4 - 2026-11-16

- Opt-in CLI tool calls (`PluginApp::allowCliTools{false}`), `RequestContext::session`, `{"input":...,"context":...}` envelope unwrapping, and `@path` / `-` (stdin) input.

## v0.1.3 - 2026-11-16

- Replace `libhv` and `slimr-curl` with `cpp-httplib[openssl]` (`httplib::Server`).
- Internal: switch macOS builds to llvm@23 with static libc++.
- Internal: harden CLI rdbuf guard and InvalidRequest test assertion.
- Internal: raise slimr-mcp line coverage to 91.18%.
- Internal: align tooling with the cpp-templates library template.
- Split `slimr-mcp` from `slimr-cpp` into its own vcpkg registry repository.
- Internal: move the repository to `slimr-down-now/mcp`
