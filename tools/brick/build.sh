#!/bin/sh
# Cross-build Rez for NextUI's tg5040 handhelds (TrimUI Brick, aarch64) into build-brick/rez.
# Needs:
#  - the toolchain (tools/brick/toolchain.sh)
#  - the device's libvulkan.so.1 (tools/brick/devlibs.sh SERIAL), to link against
#  - the PC build's translator (python tools/dc.py build), which writes the game's C++
#  - game/extracted from python tools/dc.py setup
#  - the host's Vulkan headers (vulkan-headers); the sysroot's are too old
# The NextUI build: its disc screens, and the Vulkan renderer presenting to /dev/fb0. GEN_OPT=-O2 (or -O3) compiles the translated code at that level
# instead of -O1.
set -e
cd "$(dirname "$0")/../.."
B=build-brick/rez
TC="$PWD/build-brick/toolchain"
TRANSLATE="$PWD/build/engine/translator/dream-translate"
[ -d "$TC/aarch64-nextui-linux-gnu/libc" ] || { echo "brick: no toolchain (tools/brick/toolchain.sh)"; exit 2; }
[ -x "$TRANSLATE" ] || { echo "brick: no PC translator at $TRANSLATE (python tools/dc.py build first)"; exit 2; }
VKLIB="$PWD/build-brick/devlibs/libvulkan.so.1"
[ -f "$VKLIB" ] || { echo "brick: no $VKLIB (tools/brick/devlibs.sh SERIAL)"; exit 2; }
mkdir -p build-brick/include
rm -rf build-brick/include/vulkan build-brick/include/vk_video
cp -r /usr/include/vulkan /usr/include/vk_video build-brick/include/
cmake -S . -B "$B" -G Ninja \
    -DCMAKE_CXX_FLAGS_RELEASE="-O3 -DNDEBUG" -DCMAKE_C_FLAGS_RELEASE="-O3 -DNDEBUG" \
    -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/tg5040.cmake" \
    -DDREAMCOMP_TG5040_ROOT="$TC" \
    -DCMAKE_BUILD_TYPE=Release \
    -DDREAMCOMP_NEXTUI=ON \
    -DDREAM_RENDERER=ON \
    -DVulkan_INCLUDE_DIR="$PWD/build-brick/include" \
    -DVulkan_LIBRARY="$VKLIB" \
    -DCMAKE_DISABLE_FIND_PACKAGE_SDL3=ON \
    -DDREAM_TRANSLATE_EXECUTABLE="$TRANSLATE" \
    -DDREAM_GEN_OPT="${GEN_OPT:--O1}" > "$B.configure.log"
ninja -C "$B" "$@" rez_boot
echo "brick: built $B/game/rez-recomp"
