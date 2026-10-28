cmake_minimum_required(VERSION 3.24)
project(rez_recomp VERSION 1.0.1 LANGUAGES C CXX)

# Rez (Dreamcast) as a native program. engine/ translates the game's SH-4 code to C++ at build time
# and provides the runtime it runs on; src/framework/ is the port layer (settings, NextUI screens);
# src/ replaces Sega's libraries with native code; game/rez.toml describes the game.

# Engine defaults (set before the engine's own option() calls, which then keep them).
set(DREAM_WERROR OFF CACHE BOOL "Warnings as errors in engine targets")
# The generated code is split once per hardware thread so it compiles in parallel;
# -DDREAM_GEN_SHARDS=1 keeps it in one file.
cmake_host_system_information(RESULT _dc_threads QUERY NUMBER_OF_LOGICAL_CORES)
if(_dc_threads LESS 1)
  set(_dc_threads 1)
elseif(_dc_threads GREATER 64)
  set(_dc_threads 64)
endif()
set(DREAM_GEN_SHARDS ${_dc_threads} CACHE STRING "Parts the generated game code is split into")
# SDL3 fetched by `tools/dc.py deps` (Windows) lands in .deps/; find it without flags.
file(GLOB _dc_sdl3 LIST_DIRECTORIES true "${PROJECT_SOURCE_DIR}/.deps/SDL3-*/cmake")
if(WIN32 AND _dc_sdl3 AND NOT SDL3_DIR)
  list(GET _dc_sdl3 -1 _dc_sdl3)
  set(SDL3_DIR ${_dc_sdl3} CACHE PATH "SDL3 config directory")
endif()

# NextUI handhelds (TrimUI Brick): the disc screens (SDL2, below) and the renderer's window on
# /dev/fb0 (engine/render, DREAM_RENDER_NEXTUI).
option(DREAMCOMP_NEXTUI "Build for a NextUI handheld: the disc screens before the game (SDL2)" OFF)
if(DREAMCOMP_NEXTUI)
  set(DREAM_RENDER_NEXTUI ON CACHE BOOL "" FORCE)
endif()
# Every build computes the same floating point: no implicit fused multiply-adds (clang fuses a*b+c
# by default on ARM64, which changes FTRV's and FIPR's rounding). FMAC is fused explicitly with
# std::fmaf (sh4/ops.h). The game's targets get the same in dreamcomp_add_port.
set(DREAMCOMP_FP_OPTIONS $<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-ffp-contract=off>
                         $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-ffp-contract=off>)
add_compile_options(${DREAMCOMP_FP_OPTIONS})
add_subdirectory(engine)
include(cmake/Port.cmake)
# The engine's warnings for the port layer too (dream_target_defaults also changes floating point,
# which the port layer keeps as it is).
set(DREAMCOMP_WARNINGS
    "$<$<CXX_COMPILER_ID:MSVC>:/W4>"
    "$<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-Wall;-Wextra;-Wpedantic;-Wshadow;-Wconversion;-Wno-sign-conversion>")

# The port layer linked into the game executable. An OBJECT library, because its extensions
# register themselves from static constructors that a static library would drop.
add_library(dreamcomp_core OBJECT src/framework/core.cpp src/framework/settings.cpp
                                  src/framework/hud.cpp src/framework/sharp2d.cpp)
target_include_directories(dreamcomp_core PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(dreamcomp_core PUBLIC dream::runtime dream::render)
target_compile_features(dreamcomp_core PUBLIC cxx_std_20)
target_compile_options(dreamcomp_core PRIVATE ${DREAMCOMP_WARNINGS})
if(TARGET dream_render_vk)
  target_link_libraries(dreamcomp_core PUBLIC dream::render_vk)
  target_compile_definitions(dreamcomp_core PRIVATE DREAM_WITH_RENDERER)
endif()

# NextUI handhelds: the disc screens shown before the game, drawn with the device's SDL2 and
# SDL2_ttf (cmake/toolchains/tg5040.cmake finds the sysroot's). SDL2 never goes into a PC build,
# whose SDL3 exports the same symbol names.
if(DREAMCOMP_NEXTUI)
  find_path(DREAMCOMP_SDL2_INCLUDE_DIR SDL.h PATH_SUFFIXES SDL2)
  find_library(DREAMCOMP_SDL2_LIBRARY SDL2)
  find_library(DREAMCOMP_SDL2_TTF_LIBRARY SDL2_ttf)
  if(NOT DREAMCOMP_SDL2_INCLUDE_DIR OR NOT DREAMCOMP_SDL2_LIBRARY OR NOT DREAMCOMP_SDL2_TTF_LIBRARY)
    message(FATAL_ERROR "DREAMCOMP_NEXTUI needs SDL2 and SDL2_ttf")
  endif()
  add_library(dreamcomp_nextui STATIC src/framework/nextui/disc_flow.cpp
                                      src/framework/nextui/disc_folder.cpp
                                      src/framework/nextui/font.cpp src/framework/nextui/screen.cpp)
  target_include_directories(dreamcomp_nextui PUBLIC ${PROJECT_SOURCE_DIR}/src/framework
                             PRIVATE ${DREAMCOMP_SDL2_INCLUDE_DIR})
  target_link_libraries(dreamcomp_nextui PUBLIC dream::runtime dream::translator
                        PRIVATE ${DREAMCOMP_SDL2_TTF_LIBRARY} ${DREAMCOMP_SDL2_LIBRARY})
  target_compile_features(dreamcomp_nextui PUBLIC cxx_std_20)
  target_compile_options(dreamcomp_nextui PRIVATE ${DREAMCOMP_WARNINGS})
  target_link_libraries(dreamcomp_core PUBLIC dreamcomp_nextui)
  target_compile_definitions(dreamcomp_core PRIVATE DREAMCOMP_NEXTUI)
endif()

add_subdirectory(game)
