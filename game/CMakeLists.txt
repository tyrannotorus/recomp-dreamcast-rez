dreamcomp_add_port(rez
  TITLE "Rez"
  OUTPUT_NAME rez-recomp
  SOURCES ${PROJECT_SOURCE_DIR}/src/rez.cpp ${PROJECT_SOURCE_DIR}/src/native_gfx.cpp
          ${PROJECT_SOURCE_DIR}/src/native_sys.cpp ${PROJECT_SOURCE_DIR}/src/native_snd.cpp
          ${PROJECT_SOURCE_DIR}/src/title_exit.cpp ${PROJECT_SOURCE_DIR}/src/recomp_settings.cpp
          ${PROJECT_SOURCE_DIR}/src/native_dev.cpp
          ${PROJECT_SOURCE_DIR}/src/native_gdfs.cpp
          ${PROJECT_SOURCE_DIR}/src/native_periph.cpp
          ${PROJECT_SOURCE_DIR}/src/native_sound.cpp ${PROJECT_SOURCE_DIR}/src/native_sound_driver.cpp
          ${PROJECT_SOURCE_DIR}/src/native_synth.cpp ${PROJECT_SOURCE_DIR}/src/native_irq.cpp
          ${PROJECT_SOURCE_DIR}/src/native_ninja.cpp)
