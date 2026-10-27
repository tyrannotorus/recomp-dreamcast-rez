# dream_add_game(<id> [TITLE "Name"])
#
# Everything a recompiled title needs: translate its binary at build time, compile the generated
# units, and link them with the runtime into a `<id>_boot` executable. Does nothing when the
# title's extracted binary is absent: nothing from a disc is ever committed.
#
# TITLE      what the build prints while translating; defaults to <id>.

set(DREAM_GEN_SHARDS 1 CACHE STRING
    "Split each game's generated code into this many extra units, compiled in parallel (1 = one file)")

# Cross builds (a console toolchain) cannot run the dream-translate they build: name a host build of
# it here and the game units are translated with that one instead. Empty = the target in this tree.
set(DREAM_TRANSLATE_EXECUTABLE "" CACHE FILEPATH
    "Host dream-translate to use instead of building one (cross-compiling)")

function(dream_add_game id)
  cmake_parse_arguments(ARG "" "TITLE" "" ${ARGN})
  if(NOT ARG_TITLE)
    set(ARG_TITLE ${id})
  endif()

  set(_dir ${CMAKE_CURRENT_SOURCE_DIR})
  set(_toml_path ${_dir}/${id}.toml)
  if(NOT EXISTS ${_toml_path})
    message(FATAL_ERROR "dream_add_game(${id}): no ${id}.toml in ${_dir}")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_toml_path})
  file(READ ${_toml_path} _toml)

  # The executable's location is the TOML's to decide, not this file's. Assuming
  # extracted/fs/1ST_READ.BIN here would quietly ignore a config that says otherwise.
  string(REGEX MATCH "path[ \t]*=[ \t]*\"([^\"]+)\"" _m "${_toml}")
  if(NOT CMAKE_MATCH_1)
    message(FATAL_ERROR "dream_add_game(${id}): ${id}.toml has no [binary] path")
  endif()
  set(_bin ${_dir}/${CMAKE_MATCH_1})
  if(NOT EXISTS ${_bin})
    return()  # nobody's disc here: not an error, just nothing to build
  endif()

  set(_gen ${CMAKE_CURRENT_BINARY_DIR}/gen)
  file(MAKE_DIRECTORY ${_gen})

  # One unit for the main binary, plus one per [[relocations]] entry: a region the program copies
  # somewhere else and runs from there, which has to be translated as it looks at its destination.
  set(_units ${_gen}/${id}.cpp)
  # One generated file of a few hundred MB compiles on one core for many minutes; split, the
  # parts build in parallel. 1 = a single file.
  set(_shards ${DREAM_GEN_SHARDS})
  set(_shard_flags "")
  if(_shards GREATER 1)
    set(_shard_flags --shards ${_shards})
    math(EXPR _last "${_shards} - 1")
    foreach(k RANGE 0 ${_last})
      if(k LESS 10)
        set(k "0${k}")
      endif()
      list(APPEND _units ${_gen}/${id}_part${k}.cpp)
    endforeach()
  endif()
  set(_headers ${_gen}/${id}.h)
  string(REGEX MATCHALL "dest = 0x[0-9A-Fa-f]+" _dests "${_toml}")
  set(_seen "")
  foreach(d IN LISTS _dests)
    string(REGEX REPLACE "dest = 0x" "" d "${d}")
    string(TOLOWER "${d}" d)
    # Several [[relocations]] may share a destination (overlays: the same address holding different
    # code at different times). Their later units are named _2, _3, ... exactly as
    # `dream-translate game` names them, or the build would expect files it never produces.
    list(APPEND _seen ${d})
    set(_nth 0)
    foreach(s IN LISTS _seen)
      if(s STREQUAL d)
        math(EXPR _nth "${_nth} + 1")
      endif()
    endforeach()
    if(_nth GREATER 1)
      set(d "${d}_${_nth}")
    endif()
    list(APPEND _units ${_gen}/${id}_reloc_${d}.cpp)
    list(APPEND _headers ${_gen}/${id}_reloc_${d}.h)
  endforeach()

  set(_translate dream-translate)
  if(DREAM_TRANSLATE_EXECUTABLE)
    set(_translate ${DREAM_TRANSLATE_EXECUTABLE})
  endif()
  add_custom_command(
    OUTPUT ${_units} ${_headers}
    COMMAND ${_translate} game --config ${_toml_path} --out-dir ${_gen} ${_shard_flags}
    DEPENDS ${_translate} ${_toml_path} ${_bin}
    COMMENT "Translating ${ARG_TITLE}"
    VERBATIM)

  add_executable(${id}_boot ${DREAM_ROOT}/runtime/boot/boot_main.cpp ${_units})
  target_link_libraries(${id}_boot PRIVATE dream::runtime dream::translator)
  # --window needs the Vulkan backend, which is optional (render/CMakeLists.txt returns early when
  # Vulkan, SDL3 or glslc are missing). Without it the launcher still builds and runs headless, and
  # --window says this build has no renderer rather than doing nothing.
  if(TARGET dream_render_vk)
    target_link_libraries(${id}_boot PRIVATE dream::render_vk)
    target_compile_definitions(${id}_boot PRIVATE DREAM_WITH_RENDERER)
  endif()
  # Sound is optional in the same way and independent of the renderer: a build with SDL3 but no
  # Vulkan still makes sound, and a build with neither still records through --wav.
  if(TARGET dream_audio)
    target_link_libraries(${id}_boot PRIVATE dream::audio)
    target_compile_definitions(${id}_boot PRIVATE DREAM_WITH_AUDIO)
  endif()
  dream_target_defaults(${id}_boot)
  if(NOT MSVC)
    # Hundreds of megabytes of generated C++: the optimiser stays modest by default. DREAM_GEN_OPT
    # raises it where run time matters more than build time.
    set(DREAM_GEN_OPT "-O1" CACHE STRING "Optimisation flag for the translated code")
    set_source_files_properties(${_units} PROPERTIES COMPILE_OPTIONS "${DREAM_GEN_OPT}")
  endif()
endfunction()
