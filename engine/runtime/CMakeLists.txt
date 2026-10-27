# dream_runtime: the host-side Dreamcast. Memory, scheduler, HLE and hardware cores, the
# floating-point environment layer, the SH-4 ABI and FPU helpers emitted code calls, and the
# address space (mem/).
add_library(dream_runtime STATIC
  src/fenv.cpp
  src/sh4/abi_bare.cpp
  src/sh4/fsca.cpp
  src/mem/dc_memory.cpp
  src/sched/scheduler.cpp
  src/sh4/intc.cpp
  src/sh4/tmu.cpp
  src/holly/intc.cpp
  src/holly/sysblock.cpp
  src/holly/g2dma.cpp
  src/sh4/dmac.cpp
  src/pvr/spg.cpp
  src/pvr/core.cpp
  src/system.cpp
  src/gdrom/disc.cpp
  src/gdrom/mod_disc.cpp
  src/hle/flash.cpp
  src/hle/bios.cpp
  src/maple/maple.cpp
  src/aica/aica.cpp
  src/aica/mixer.cpp
)
target_include_directories(dream_runtime PUBLIC include)
# libchdr (BSD-3, third_party) for CHD disc images; the static target avoids a runtime DLL search.
target_include_directories(dream_runtime SYSTEM PUBLIC ${DREAM_ROOT}/third_party/libchdr/include)
target_link_libraries(dream_runtime PUBLIC chdr-static)
dream_target_defaults(dream_runtime)
add_library(dream::runtime ALIAS dream_runtime)
