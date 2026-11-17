# Changelog

## Unreleased

## v0.2.2 - 2026-11-17

- feat: default-on /tools surface, OAuth audience parity, and slimr-serve v0.3.1 (#1)
  * feat: use VCPKG_OVERLAY_PORTS and guard release
  * chore: adopt PR CI/CD and contributing templates
  * docs: maintainer setup for release App, secrets, rulesets
  * docs: soft alignment marker and release-bot contributing docs
  * feat: default-on /tools surface, McpHttpOptions::tools opt-out, MRTR error, OAuth audience parity
  - Remove PluginApp --openapi test case: --openapi flag removed in slimr-serve
  - Replace CLI direct invocation, allowCliTools/envelope, and CLI run tool-call assertions with CLI rejection tests: per-tool CLI removed
  - Update default host assertion (127.0.0.1) and /tools/schema path in HTTP tests to match slimr-serve and /tools prefix
  * build: bump slimr-serve baseline to v0.3.1
- ci: poll gh run watch every 30s in release

## v0.2.1 - 2026-11-17

- Forward OAuth OpenAPI defaults in `enable_http`, test `GET /openapi.json` and `--openapi`, and bump `slimr-serve` to `>= 0.2.0`.
- Docs: document `GET /openapi.json`, `--openapi`, and overlay-port development workflow in README.
- Internal: fix broken docs link in README.

## v0.2.0 - 2026-11-16

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
