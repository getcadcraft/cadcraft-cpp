# Security

This is a test version of CadCraft. It opens files from strangers (DWG, DXF, and later SKP, IFC, PDF), so parser
bugs are security bugs.

## Reporting

Please report a vulnerability privately through GitHub's "Report a vulnerability" (Security tab of
github.com/getcadcraft/cadcraft-cpp), not in a public issue. Include the file or steps that trigger it. We answer
within a week.

## What the code does about it

- Every reader goes through one bounds-checked reader (`libs/base`), with size, count and depth limits.
- The standard library is hardened in every build; the compiler hardening flags are on (AGENTS.md section 4).
- The whole test suite runs under ASan + UBSan and TSan in CI; every parser has a libFuzzer target run on every push.
- Binary formats from strangers are parsed in a separate helper process on the desktop, so a parser crash costs a
  message, not your open work.
- Dependencies: licence allowlist and an OSV vulnerability check in CI (`tools/licenses.cmake`, `tools/osv.cmake`);
  waivers, if any, are in `docs/security-waivers.md` with a reason and a date.
