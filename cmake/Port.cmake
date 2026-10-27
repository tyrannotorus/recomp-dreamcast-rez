# dreamcomp_add_port(<id> TITLE <title> [OUTPUT_NAME <exe name>] [SOURCES <sources>...])
#
# Wraps the engine's dream_add_game() (translate the extracted boot executable and overlays, build
# <id>_boot) and adds the port layer and the game's own sources to that executable. A no-op until
# the disc has been extracted with `python tools/dc.py setup --disc <image>`.
function(dreamcomp_add_port id)
  cmake_parse_arguments(ARG "" "TITLE;OUTPUT_NAME" "SOURCES" ${ARGN})
  if(NOT ARG_TITLE)
    set(ARG_TITLE ${id})
  endif()
  # The same floating point as the engine, for every target made in the calling directory from
  # here on, the generated code included.
  add_compile_options($<$<COMPILE_LANG_AND_ID:CXX,GNU,Clang,AppleClang>:-ffp-contract=off>
                      $<$<COMPILE_LANG_AND_ID:C,GNU,Clang,AppleClang>:-ffp-contract=off>)
  dream_add_game(${id} TITLE "${ARG_TITLE}")
  if(NOT TARGET ${id}_boot)
    message(STATUS "${ARG_TITLE}: no extracted boot executable yet; run "
                   "`python tools/dc.py setup --disc <image>`")
    return()
  endif()
  target_link_libraries(${id}_boot PRIVATE dreamcomp_core)
  if(ARG_SOURCES)
    target_sources(${id}_boot PRIVATE ${ARG_SOURCES})
  endif()
  # A linker map beside the executable, so a crash report's "module + offset" names a function.
  if(MSVC)
    target_link_options(${id}_boot PRIVATE "/MAP")
  endif()
  if(ARG_OUTPUT_NAME)
    set_target_properties(${id}_boot PROPERTIES OUTPUT_NAME ${ARG_OUTPUT_NAME})
  endif()
  target_compile_definitions(${id}_boot PRIVATE DREAMCOMP_PORT_ID="${id}"
                                                DREAMCOMP_PORT_TITLE="${ARG_TITLE}")
  # Windows has no rpath: put the runtime DLLs of everything linked (SDL3) next to the executable.
  if(WIN32)
    add_custom_command(TARGET ${id}_boot POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_RUNTIME_DLLS:${id}_boot>
              $<TARGET_FILE_DIR:${id}_boot>
      COMMAND_EXPAND_LISTS VERBATIM)
  endif()
  message(STATUS "${ARG_TITLE} (${id}) configured")
endfunction()
