cmake_minimum_required(VERSION 3.16)
project(utdc_builder C RC)

set(UTDC_VERSION "0.1.1" CACHE STRING "Version string")
set(UTDC_PAYLOAD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../runner/build-dc" CACHE PATH "Folder with 1ST_READ.BIN and IP.BIN")

set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

set(PAYLOAD_C ${CMAKE_CURRENT_BINARY_DIR}/payload.c)
set(PAYLOAD_DEPS "")
foreach(f 1ST_READ.BIN IP.BIN)
  if(EXISTS ${UTDC_PAYLOAD_DIR}/${f})
    list(APPEND PAYLOAD_DEPS ${UTDC_PAYLOAD_DIR}/${f})
  else()
    message(WARNING "${UTDC_PAYLOAD_DIR}/${f} not found - the builder will contain no Dreamcast runner")
  endif()
endforeach()
add_custom_command(
  OUTPUT ${PAYLOAD_C}
  COMMAND ${CMAKE_COMMAND} -DIN_BOOT=${UTDC_PAYLOAD_DIR}/1ST_READ.BIN -DIN_IP=${UTDC_PAYLOAD_DIR}/IP.BIN -DOUT=${PAYLOAD_C} -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bin2c.cmake ${PAYLOAD_DEPS}
  COMMENT "Embedding Dreamcast runner payload")

set(CORE_SRC
  src/build.c src/datawin.c src/textures.c src/vq.c src/audio.c src/disc.c src/impl.c
  third_party/miniz/miniz.c third_party/miniz/miniz_tdef.c third_party/miniz/miniz_tinfl.c
  third_party/lz4/lz4.c third_party/lz4/lz4hc.c
  ${PAYLOAD_C})

add_library(utdc_core STATIC ${CORE_SRC})
target_compile_definitions(utdc_core PUBLIC UTDC_VERSION="${UTDC_VERSION}" MINIZ_NO_ZLIB_COMPATIBLE_NAMES _CRT_SECURE_NO_WARNINGS)
target_include_directories(utdc_core PUBLIC src third_party/miniz)
if(NOT MSVC)
  target_link_libraries(utdc_core PUBLIC m)
  target_compile_options(utdc_core PRIVATE -O2 -Wall -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable)
endif()

add_executable(utdc-builder-cli src/cli_main.c)
target_link_libraries(utdc-builder-cli utdc_core)

if(WIN32)
  add_executable(UTDC-Wizard WIN32 src/win_main.c src/wizard.rc)
  target_link_libraries(UTDC-Wizard utdc_core comctl32 shell32 ole32 comdlg32)
  if(MINGW)
    target_link_options(UTDC-Wizard PRIVATE -static -municode)
    target_link_options(utdc-builder-cli PRIVATE -static)
  endif()
endif()
