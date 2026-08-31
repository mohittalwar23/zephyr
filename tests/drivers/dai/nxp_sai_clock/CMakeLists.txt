# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(nxp_sai_clock)

target_include_directories(app PRIVATE ${ZEPHYR_BASE}/drivers/dai/nxp/sai)
target_sources(app PRIVATE src/main.c)
set_source_files_properties(src/main.c PROPERTIES COMPILE_OPTIONS -Wno-unused-function)

add_custom_target(verify_sai_providers ALL
  COMMAND ${PYTHON_EXECUTABLE}
          ${CMAKE_CURRENT_SOURCE_DIR}/verify_providers.py
          --readelf ${CMAKE_READELF}
          --nm ${CMAKE_NM}
          --app-obj ${PROJECT_BINARY_DIR}/CMakeFiles/app.dir/src/main.c.obj
          --sai-obj ${PROJECT_BINARY_DIR}/zephyr/drivers/dai/nxp/sai/CMakeFiles/drivers__dai__nxp__sai.dir/sai.c.obj
  DEPENDS app
  VERBATIM
)
