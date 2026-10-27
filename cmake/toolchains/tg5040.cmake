# Cross-build for NextUI's tg5040 handhelds (TrimUI Brick and Smart Pro: aarch64 Cortex-A53) with the
# host's clang and lld, against the sysroot and GCC 8.3 runtime of LoveRetro's tg5040 toolchain.
# DREAMCOMP_TG5040_ROOT names the toolchain's root, the folder holding aarch64-nextui-linux-gnu/
# (tools/brick/toolchain.sh copies it out of the toolchain image).
#   cmake -S . -B <build> -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/tg5040.cmake \
#         -DDREAMCOMP_TG5040_ROOT=<root> -DDREAMCOMP_NEXTUI=ON ...
set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
if(NOT DREAMCOMP_TG5040_ROOT AND DEFINED ENV{DREAMCOMP_TG5040_ROOT})
  set(DREAMCOMP_TG5040_ROOT $ENV{DREAMCOMP_TG5040_ROOT})
endif()
if(NOT EXISTS "${DREAMCOMP_TG5040_ROOT}/aarch64-nextui-linux-gnu/libc")
  message(FATAL_ERROR "tg5040: DREAMCOMP_TG5040_ROOT (${DREAMCOMP_TG5040_ROOT}) is not the toolchain's root")
endif()
# try_compile projects get the toolchain file again; pass the root on to them.
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES DREAMCOMP_TG5040_ROOT)
set(CMAKE_SYSROOT "${DREAMCOMP_TG5040_ROOT}/aarch64-nextui-linux-gnu/libc")
set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_C_COMPILER_TARGET aarch64-nextui-linux-gnu)
set(CMAKE_CXX_COMPILER_TARGET aarch64-nextui-linux-gnu)
# Assembly too (libchdr's LZMA decoder has an arm64 one); without a target clang assembles for the host.
set(CMAKE_ASM_COMPILER clang)
set(CMAKE_ASM_COMPILER_TARGET aarch64-nextui-linux-gnu)
set(_tg5040_flags "--gcc-toolchain=${DREAMCOMP_TG5040_ROOT} -mcpu=cortex-a53")
set(CMAKE_C_FLAGS_INIT "${_tg5040_flags}")
set(CMAKE_CXX_FLAGS_INIT "${_tg5040_flags}")
set(CMAKE_ASM_FLAGS_INIT "${_tg5040_flags}")
# The device's libstdc++ is older than the toolchain's headers may assume: link the C++ runtime in.
set(CMAKE_EXE_LINKER_FLAGS_INIT "${_tg5040_flags} -fuse-ld=lld -static-libstdc++ -static-libgcc")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${_tg5040_flags} -fuse-ld=lld -static-libstdc++ -static-libgcc")
# GCC 8's std::filesystem lives in a library of its own, and glibc 2.28's threads too (before 2.34).
set(CMAKE_CXX_STANDARD_LIBRARIES "-lstdc++fs -lpthread")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)
