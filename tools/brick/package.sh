#!/bin/sh
# Assemble build-brick/Rez.pak from the Brick build (tools/brick/build.sh) and pak/: the folder
# NextUI runs as Tools > Rez. The executable contains the game's translated code; the player's own
# CHD, read from disc/ on the device, is never packaged.
set -e
cd "$(dirname "$0")/../.."
EXE=build-brick/rez/game/rez-recomp
PAK=build-brick/Rez.pak
[ -x "$EXE" ] || { echo "package: $EXE missing (tools/brick/build.sh)"; exit 2; }
rm -rf "$PAK"
mkdir -p "$PAK/disc" "$PAK/licenses"
cp pak/launch.sh "$PAK/launch.sh"
llvm-strip -o "$PAK/rez" "$EXE"
cp game/rez.toml "$PAK/rez.toml"
cp game/rez-pipelines.txt "$PAK/rez-pipelines.txt"
chmod +x "$PAK/launch.sh" "$PAK/rez"
cp LICENSE "$PAK/licenses/rez-recomp-GPL-2.0.txt"
cp THIRD_PARTY_NOTICES.md "$PAK/licenses/THIRD_PARTY_NOTICES.md"
cp engine/third_party/libchdr/LICENSE.txt "$PAK/licenses/libchdr-BSD-3-Clause.txt"
cp engine/third_party/libchdr/deps/lzma-26.02/LICENSE "$PAK/licenses/lzma.txt"
cp engine/third_party/tomlplusplus/LICENSE "$PAK/licenses/tomlplusplus-MIT.txt"
cp assets/fonts/OFL.txt "$PAK/licenses/NotoSans-OFL-1.1.txt"
echo "package: $PAK ($(du -sh "$PAK" | cut -f1))"
# The download: the pak's contents at the zip's root, as NextUI's Pak Store lays them out, with the
# files' modes kept and the empty disc/ folder included.
rm -f "$PAK.zip"
python3 - "$PAK" "$PAK.zip" <<'EOF'
import os, sys, zipfile
src, dst = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
    for root, dirs, files in os.walk(src):
        dirs.sort()
        if root != src and not files and not dirs:
            z.write(root, os.path.relpath(root, src) + "/")
        for name in sorted(files):
            path = os.path.join(root, name)
            z.write(path, os.path.relpath(path, src))
EOF
echo "package: $PAK.zip ($(du -h "$PAK.zip" | cut -f1))"
