# dream::render: the Vulkan renderer. Optional: without Vulkan and SDL3 everything else still
# builds (headless). Split in two:
#   dream_render       device-independent: display-list decoding and texture decoding.
#   dream_render_vk    the Vulkan backend and the window. Built only when the dependencies are
#                      present.
add_library(dream_render STATIC
  src/display_list.cpp
  src/texture.cpp
  src/texture_pack.cpp
  src/png.cpp
  src/framebuffer.cpp
  src/background.cpp
  src/fog.cpp
  src/overlay.cpp
  src/interpolate.cpp
  src/input.cpp
  src/input_menu.cpp
)
target_include_directories(dream_render PUBLIC include)
target_link_libraries(dream_render PUBLIC dream::runtime)
dream_target_defaults(dream_render)
add_library(dream::render ALIAS dream_render)

if(NOT DREAM_RENDERER)
  return()
endif()

# A NextUI handheld's window presents to /dev/fb0 and reads the pad's input device itself
# (src/vk/window_nextui.cpp, src/vk/fb0_present.cpp): no SDL3.
option(DREAM_RENDER_NEXTUI "Vulkan window for NextUI handhelds: /dev/fb0, no SDL3" OFF)
find_package(Vulkan QUIET)
if(NOT DREAM_RENDER_NEXTUI)
  find_package(SDL3 QUIET CONFIG)
endif()
if(NOT Vulkan_FOUND OR (NOT DREAM_RENDER_NEXTUI AND NOT SDL3_FOUND))
  message(STATUS "dream::render: Vulkan ${Vulkan_FOUND} SDL3 ${SDL3_FOUND}; the Vulkan backend is skipped")
  return()
endif()

# Shaders are compiled to SPIR-V at build time and embedded, so a release build carries no shader
# source and needs no compiler at run time. glslc ships with the Vulkan SDK.
find_program(DREAM_GLSLC NAMES glslc HINTS ${Vulkan_GLSLC_EXECUTABLE} "$ENV{VULKAN_SDK}/bin")
if(NOT DREAM_GLSLC)
  message(STATUS "dream::render: no glslc; the Vulkan backend is skipped")
  return()
endif()

set(_spv_dir ${CMAKE_CURRENT_BINARY_DIR}/shaders)
file(MAKE_DIRECTORY ${_spv_dir})
set(_spv_headers)
foreach(_shader geometry.vert geometry.frag present.vert present.frag modvol.vert modvol.frag
                lines.vert)
  string(REPLACE "." "_" _sym ${_shader})
  set(_out ${_spv_dir}/${_sym}.h)
  # glslc -mfmt=c emits a bare initialiser list; wrap it in a declaration so the header can just
  # be included.
  add_custom_command(
    OUTPUT ${_out}
    COMMAND ${DREAM_GLSLC} --target-env=vulkan1.1 -O
            -mfmt=c -o ${_out}.inc ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader}
    COMMAND ${CMAKE_COMMAND} -DIN=${_out}.inc -DOUT=${_out} -DSYM=${_sym}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wrap_spirv.cmake
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${_shader}
            ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wrap_spirv.cmake
    COMMENT "glslc ${_shader}"
    VERBATIM)
  list(APPEND _spv_headers ${_out})
endforeach()
# The geometry fragment shader without its discards, for the pipelines that need neither
# (renderer.h, PipelineKey::discard).
set(_out ${_spv_dir}/geometry_frag_nd.h)
add_custom_command(
  OUTPUT ${_out}
  COMMAND ${DREAM_GLSLC} --target-env=vulkan1.1 -O -DNO_DISCARD=1
          -mfmt=c -o ${_out}.inc ${CMAKE_CURRENT_SOURCE_DIR}/shaders/geometry.frag
  COMMAND ${CMAKE_COMMAND} -DIN=${_out}.inc -DOUT=${_out} -DSYM=geometry_frag_nd
          -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wrap_spirv.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/geometry.frag
          ${CMAKE_CURRENT_SOURCE_DIR}/cmake/wrap_spirv.cmake
  COMMENT "glslc geometry.frag (NO_DISCARD)"
  VERBATIM)
list(APPEND _spv_headers ${_out})
add_custom_target(dream_render_shaders DEPENDS ${_spv_headers})

if(DREAM_RENDER_NEXTUI)
  set(_window_src src/vk/window_nextui.cpp)
else()
  set(_window_src src/vk/window.cpp)
endif()
add_library(dream_render_vk STATIC
  src/vk/context.cpp
  ${_window_src}
  src/vk/fb0_present.cpp
  src/vk/resources.cpp
  src/vk/renderer.cpp
  src/vk/texture_cache.cpp
  src/vk/offscreen.cpp
  src/vk/present.cpp
  src/vk/dxgi_present.cpp
)
add_dependencies(dream_render_vk dream_render_shaders)
target_include_directories(dream_render_vk PUBLIC include PRIVATE ${_spv_dir})
target_link_libraries(dream_render_vk PUBLIC dream::render Vulkan::Vulkan)
if(DREAM_RENDER_NEXTUI)
  target_compile_definitions(dream_render_vk PRIVATE DREAM_RENDER_FB0)
else()
  target_link_libraries(dream_render_vk PUBLIC SDL3::SDL3)
endif()
if(WIN32)
  # DXGI flip-model presentation (src/vk/dxgi_present.cpp).
  target_link_libraries(dream_render_vk PRIVATE d3d11 dxgi)
endif()
dream_target_defaults(dream_render_vk)
add_library(dream::render_vk ALIAS dream_render_vk)

if(DREAM_RENDER_NEXTUI)
  message(STATUS "dream::render: Vulkan backend enabled for NextUI (fb0; glslc ${DREAM_GLSLC})")
else()
  message(STATUS "dream::render: Vulkan backend enabled (glslc ${DREAM_GLSLC})")
endif()
