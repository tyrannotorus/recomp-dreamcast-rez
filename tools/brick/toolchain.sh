#!/bin/sh
# The tg5040 cross toolchain (NextUI: aarch64, glibc 2.28, GCC 8.3 runtime), copied out of
# LoveRetro's tg5040-toolchain image into build-brick/toolchain. The image must already be pulled
# (docker pull ghcr.io/loveretro/tg5040-toolchain:latest); no code from it runs (docker create +
# docker cp). The host's clang and lld compile against its sysroot (cmake/toolchains/tg5040.cmake).
# Tested with image sha256:f131c6af64029a8723d0ce8d3c2682642f5f091b04714f6beedda9bec18477ab.
set -e
cd "$(dirname "$0")/../.."
IMG=ghcr.io/loveretro/tg5040-toolchain:latest
OUT=build-brick/toolchain
if [ -d "$OUT/aarch64-nextui-linux-gnu/libc" ]; then
    echo "toolchain: $OUT is there"
    exit 0
fi
docker image inspect "$IMG" > /dev/null 2>&1 || { echo "toolchain: the image $IMG is not on this machine (docker pull $IMG)"; exit 2; }
echo "toolchain: image $(docker image inspect "$IMG" --format '{{.Id}}')"
mkdir -p "$OUT.part"
CID=$(docker create --pull=never "$IMG" /bin/true)
trap 'docker rm "$CID" > /dev/null' EXIT
# Through tar: the image's directories are read-only, and docker cp to a folder makes each one
# before its contents; tar sets a directory's mode after filling it.
docker cp "$CID:/opt/aarch64-nextui-linux-gnu" - | tar -x -C "$OUT.part"
mv "$OUT.part/aarch64-nextui-linux-gnu" "$OUT"
rmdir "$OUT.part"
echo "toolchain: $OUT ($(du -sh "$OUT" | cut -f1))"
