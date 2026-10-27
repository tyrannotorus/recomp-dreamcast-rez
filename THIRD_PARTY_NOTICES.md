# Third-party notices

| Component | Where | Licence | Notes |
|---|---|---|---|
| dream-recomp | `engine/` | GPL-2.0 | phobos665, <https://github.com/phobos665/dream-recomp> (from commit `51a148c`), modified |
| dreamcomp | `src/framework/`, `include/dreamcomp/`, `cmake/`, `tools/dc.py`, and changes throughout `engine/` | GPL-2.0 | <https://github.com/cherryduck/dreamcomp>, branch `linux-ecco` (from commit `35e529a`), modified |
| Flycast (portions, via dream-recomp) | `engine/runtime/` (AICA mixer, hardware models) | GPL-2.0 | flyinghead and contributors, <https://github.com/flyinghead/flycast> |
| libchdr | `engine/third_party/libchdr/` | BSD-3-Clause | rtissera, <https://github.com/rtissera/libchdr> (commit `8e7b8bd`); bundles LZMA SDK (public domain), miniz (MIT) and zstd (BSD-3-Clause or GPL-2.0) |
| toml++ 3.4.0 | `engine/third_party/tomlplusplus/` | MIT | Mark Gillard |
| Noto Sans Bold | `assets/fonts/NotoSans-Bold.ttf`, compiled into the NextUI build | SIL OFL 1.1 | The Noto Project Authors; licence in `assets/fonts/OFL.txt` |
| SDL3 | the system's, or `.deps/` on Windows (`tools/dc.py deps`); not committed | zlib | PC builds |
| SDL2, SDL2_ttf | the NextUI device's own, linked dynamically, never shipped | zlib | NextUI disc screens and sound |
| Vulkan loader | the system's | Apache-2.0 | |
