# Third-party code

| Component | Where | License |
|---|---|---|
| Butterscotch (GameMaker runner) | `runner/` | AGPL-3.0 |
| KallistiOS (Dreamcast SDK, built by `toolchain/build-toolchain.sh`) | not vendored; one patch in `toolchain/patches/` | BSD (KOS license) |
| GCC / binutils / newlib (toolchain) | downloaded at build time | GPL / LGPL / BSD |
| stb_image, stb_vorbis | `builder/third_party/stb` | Public domain / MIT |
| miniz | `builder/third_party/miniz`, `runner/vendor/miniz` | MIT |
| LZ4 / LZ4-HC | `builder/third_party/lz4` | BSD-2-Clause |
| AICA ADPCM encoder algorithm (superctr, via KOS `wav2adpcm`) | `builder/src/audio.c` | Public domain |

No game assets are included in this repository. The wizard reads them from the user's own
installed copy of the game at build time.
