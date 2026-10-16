# Assets

Last updated 2026-10-16 · Change: Archivo (the 0.5 drawing and UI font); the 0.5 CadCraft language catalogs.

Every file under `assets/` has a row here (`tools/assets.cmake` fails the gate otherwise). Original, OSI-licensed
or CC0 only; fonts OFL only (AGENTS.md section 4, Assets).

| File | Source | Licence | Notes |
|---|---|---|---|
| `assets/fonts/archivo/Archivo-VF.ttf` | google/fonts `ofl/archivo/Archivo[wdth,wght].ttf` at commit 95f4904fc8bcf26d3420fe315560c96417c6dec7 (git blob cc64253d; sha256 0e094a7d3c7c4c25cf1310c4b30014f1dae9332220b1c2c88f4fa996f0b05053), renamed | OFL-1.1 | Variable (width, weight). The font CadCraft 0.5 uses for its UI and draws drawing text with ("Archivo Narrow" = the wdth axis) |
| `assets/fonts/archivo/OFL.txt` | same commit, `ofl/archivo/OFL.txt` | OFL-1.1 | The licence, shipped beside the font |
| `assets/lang/en.json` | github.com/getcadcraft/cadcraft `app/lang/en.json` at commit b6f3aa19078aef8b687448fbc26f38e45af5d45b (git blob c361aafd, identical to the frozen 0.5 build; sha256 533c4ccdc89f365ae7af6747b35de8963039cf164fdb9809dd23bd0e89ed1222) | Apache-2.0 (CadCraft) | CadCraft 0.5's UI words, keys unchanged (CPP-AGENTS.md section 15) |
| `assets/lang/es-MX.json` | same commit, `app/lang/es-MX.json` (git blob 33f2df33; sha256 a7c9448289aa447de2793ebaf17495811f5e417de6c5def7791a8c62a0967f39) | Apache-2.0 (CadCraft) | Spanish (Mexico); ships as a draft, as in 0.5 |
