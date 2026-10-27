#!/bin/sh
# The Brick's own Vulkan loader (libvulkan.so.1), pulled over adb into build-brick/devlibs so the
# Brick build can link against it; the toolchain's sysroot has none. Never shipped: the device has
# it in /usr/lib.
#   tools/brick/devlibs.sh SERIAL        (adb devices -l)
set -e
cd "$(dirname "$0")/../.."
SERIAL=${1:?usage: tools/brick/devlibs.sh SERIAL}
mkdir -p build-brick/devlibs
adb -s "$SERIAL" pull /usr/lib/libvulkan.so.1 build-brick/devlibs/libvulkan.so.1 > /dev/null
echo "devlibs: build-brick/devlibs/libvulkan.so.1 ($(wc -c < build-brick/devlibs/libvulkan.so.1) bytes)"
