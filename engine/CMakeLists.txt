cmake_minimum_required(VERSION 3.24)
project(dream_recomp VERSION 0.1.0 LANGUAGES C CXX)

# The recompiler and runtime: static recompilation of Sega Dreamcast code.

# Where the engine's own sources are (not CMAKE_SOURCE_DIR: the repository root is the top-level
# project). Cached rather than a plain variable because a sibling add_subdirectory(), as the
# repository adds game/, does not see normal variables set in here.
set(DREAM_ROOT ${CMAKE_CURRENT_SOURCE_DIR} CACHE INTERNAL "engine source root")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(DREAM_WERROR "Treat warnings as errors in dream targets" ON)
option(DREAM_RENDERER
  "Build the Vulkan renderer and the SDL3 window. Needs the Vulkan SDK and SDL3; without
   them the rest of the project still builds." ON)

include(cmake/DreamTargetDefaults.cmake)
include(cmake/DreamAddGame.cmake)

# libchdr (BSD-3) for CHD disc images, built shared so tools/dcdisc can load it via ctypes.
function(dream_add_libchdr)
  # libchdr declares cmake_minimum_required(3.10), under which option() ignores a normal variable
  # of the same name (CMP0077 OLD). Force NEW so the settings below actually apply.
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  set(BUILD_SHARED_LIBS ON)
  set(CHDR_WANT_TESTS OFF)
  set(INSTALL_STATIC_LIBS OFF)
  add_subdirectory(third_party/libchdr)
endfunction()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libchdr/CMakeLists.txt")
  dream_add_libchdr()
else()
  message(FATAL_ERROR "third_party/libchdr is missing")
endif()

add_subdirectory(runtime)
# toml++ (MIT, third_party/tomlplusplus): per-game configuration files. Header-only.
add_library(dream_toml INTERFACE)
target_include_directories(dream_toml SYSTEM INTERFACE ${DREAM_ROOT}/third_party/tomlplusplus)
target_compile_definitions(dream_toml INTERFACE TOML_EXCEPTIONS=0)
add_library(dream::toml ALIAS dream_toml)

add_subdirectory(render)
add_subdirectory(audio)
add_subdirectory(translator)

message(STATUS "engine: renderer ${DREAM_RENDERER}, "
               "host ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}")
