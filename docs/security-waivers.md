# Security waivers

Last updated 2026-10-20 · Change: CVE-2026-85091 (zlib gz_vacate).

A known vulnerability in a dependency fails `gate.osv` unless its id is listed here with the reason it does not
apply and the date. Each waiver is dropped once a fixed version is in the vcpkg baseline.

| Id | Dependency | Why it does not apply | Date |
|---|---|---|---|
| CVE-2026-85091 | zlib 1.3.2 (vcpkg) | A heap overflow in `gz_vacate()`, reached through `gzprintf()` / `gzvprintf()` after a non-blocking `gzwrite()` stall: the gz* file API. CadCraft calls only `compress2` / `inflate` on memory (libs/image: PNG, PDF streams, pictures); FreeType reads gzip-compressed fonts through `inflate` and only our bundled fonts. Nothing calls a gz* function (`grep -rnE 'gz(open\|write\|printf)' libs apps fuzz` is empty). Revisit when zlib 1.3.3 reaches the baseline. | 2026-10-20 |
