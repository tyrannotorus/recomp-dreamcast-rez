# Building Rez Recompiled

## Linux

Needs CMake 3.24+, Ninja, a C++20 compiler (GCC or Clang), Python 3.11+, SDL3, and the Vulkan
headers, loader and `glslc`.

```
python tools/dc.py setup --disc "/path/to/Rez (Japan) (En,Ja,Fr,De,Es,It).chd"
python tools/dc.py build
python tools/dc.py run --window
```

`setup` checks the image (its CHD SHA-1, then all of its data against the CHD's own checksum) and
the boot file's SHA-1. It remembers the image's path, so leave the image where it is. Settings and
the memory card are kept in `~/.config/rez_recompiled/`.

## TrimUI Brick pak

Needs, in addition: Docker with LoveRetro's `ghcr.io/loveretro/tg5040-toolchain` image pulled,
Clang, LLD and `llvm-strip`, and `adb` with a Brick connected once (to copy its Vulkan loader to link
against). Build the PC version first: it provides the translator.

```
tools/brick/toolchain.sh            # copies the toolchain out of the image
tools/brick/devlibs.sh <adb serial> # copies the Brick's libvulkan.so.1
tools/brick/build.sh
tools/brick/package.sh              # writes build-brick/Rez.pak and Rez.pak.zip
```

Install as in the [README](README.md#install-on-the-trimui-brick-nextui).

Executables built from this repository, the Brick pak included, contain the game's code in
translated form, compiled from the builder's own disc. They still need the player's disc to run.
