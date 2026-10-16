# Third-party code

Last updated 2026-10-16 · Change: first version.

Every dependency, why it is here, its licence and how its CVE history was checked (AGENTS.md section 3). Versions
are pinned by the vcpkg baseline in `vcpkg.json` (docs/architecture.md, "Toolchain"). `tools/licenses.cmake` checks
the licence of every installed port; `tools/osv.cmake` asks api.osv.dev about each port's upstream tag.

| Dependency | Version | Licence | Used by | Why | Checked |
|---|---|---|---|---|---|
| Catch2 | 3.15.3 | BSL-1.0 | tests only | unit tests | osv.dev: no known vulnerabilities (2026-10-16) |
