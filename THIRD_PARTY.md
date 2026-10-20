# Third-party code

Last updated 2026-10-18 · Change: V8's sin / cos (fdlibm) in libs/base and earcut (ISC) in libs/m3d (M4.4).

Every dependency, why it is here, its licence and how its CVE history was checked (AGENTS.md section 3). Versions
are pinned by the vcpkg baseline in `vcpkg.json` (docs/architecture.md, "Toolchain"). `tools/licenses.cmake` checks
the licence of every installed port; `tools/osv.cmake` asks api.osv.dev about each port's upstream tag.

| Dependency | Version | Licence | Used by | Why | Checked |
|---|---|---|---|---|---|
| Catch2 | 3.15.3 | BSL-1.0 | tests only | unit tests | osv.dev: no known vulnerabilities (2026-10-16) |
| SDL3 | 3.4.12 (vcpkg) | Zlib | ui-imgui, apps | window, input, file dialogs, drops; **no default features**: `dbus`/`ibus` pull in libsystemd (LGPL-2.1) and dbus (AFL-2.1/GPL), outside the allowlist, so Linux file dialogs fall back to zenity and IME waits (docs/gaps.md) | gate.osv |
| Dawn | 20260714.215939 (vcpkg; emdawnwebgpu on the web) | BSD-3-Clause | ui-imgui, apps | WebGPU: Vulkan (Linux, Windows), D3D11 (Windows), Metal (macOS); **not D3D12**: it needs DXC (NCSA), outside the allowlist | gate.osv |
| abseil, vulkan-headers, vulkan-utility-libraries | 20260107.1, 1.4.350.1 (vcpkg, for Dawn) | Apache-2.0 (headers: Apache-2.0 OR MIT) | Dawn | vulkan-utility-libraries declares no SPDX licence in its port: Apache-2.0 per its copyright file, allowed by hand in tools/licenses.cmake | gate.osv |
| FreeType | 2.14.3 (vcpkg; features brotli, zlib) | FTL | text | our bundled fonts only (never a document's), SDF glyphs | gate.osv |
| brotli, zlib | 1.2.0, 1.3.2 (vcpkg; zlib a base dependency since M5.8c) | MIT, Zlib | FreeType (WOFF2), image (PNG, PDF streams, pictures: 0.5's own PNG decoder) | CVE-2026-85091 waived (docs/security-waivers.md: the gz* file API is never called) | gate.osv |
| glm | 1.0.3 (vcpkg) | MIT | (3D, M5) | | gate.osv |
| Dear ImGui | v1.92.8-docking, commit b61e56346a92cfcaf1f43a545ca37b0b32239654, sha256 100c1e0a…414a4 (cmake/ImGui.cmake, FetchContent) | MIT | ui-imgui | the UI; fetched, not from vcpkg (its sdl3/webgpu bindings depend on SDL3 and Dawn with default features: the LGPL/NCSA code above) | osv.dev query by hand for github.com/ocornut/imgui: no known vulnerabilities (2026-10-16); not covered by gate.licenses/osv (not a vcpkg port) |
| V8 `src/base/ieee754.cc` (atan, atan2, sin, cos with fdlibm's argument reduction), adapted from fdlibm | V8 12.4.254.21 (the algorithms have not changed since 2016) | BSD-3-Clause (V8) + the fdlibm notice (SunPro: "Permission to use, copy, modify, and distribute this software is freely granted, provided that this notice is preserved") | base (`js_math.cpp`, notices kept in the file) | Math.atan2 / Math.sin / Math.cos to JavaScript's last bit: the C library's differs in the last bit on 15 % of atan2 inputs and 2.5 % of sin inputs, and a saved DWG keeps every bit; atan2 checked bit for bit against Node on 2,000,000 pairs, sin and cos on 3,000,000 inputs (to 1e300) | source copied by hand, not a package: no CVEs apply to pure functions |
| earcut (polygon triangulation), as CadCraft 0.5's m3d/earcut.js follows it | mapbox/earcut, algorithm as of v2.2 (the z-order hash left out, as in the JS) | ISC (© Mapbox; the notice is kept in `libs/m3d/src/earcut.cpp`) | m3d (`earcut.cpp`) | faceTris: the 3D model's faces as triangles, the same triangles as the JS | ported by hand from our own JS (no package): no CVEs apply |
