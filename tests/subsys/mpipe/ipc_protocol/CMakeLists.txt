# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mpipe_ipc_protocol_test)

# The checked-in JSON vectors are the cross-language source of truth. Generate
# the byte arrays at build time so the C tests can never drift from the file a
# Python peer would read.
set(vector_json ${CMAKE_CURRENT_LIST_DIR}/vectors/mpipe-ipc-v1.json)
set(vector_script ${CMAKE_CURRENT_LIST_DIR}/scripts/vector_to_header.py)
set(vector_header ${CMAKE_CURRENT_BINARY_DIR}/vectors/mpipe_ipc_vectors.h)

add_custom_command(
  OUTPUT ${vector_header}
  COMMAND ${PYTHON_EXECUTABLE} ${vector_script}
          --input ${vector_json} --output ${vector_header}
  DEPENDS ${vector_json} ${vector_script}
  COMMENT "Generating MIPC golden-vector header"
  VERBATIM
)
add_custom_target(mpipe_ipc_vectors_header DEPENDS ${vector_header})

file(GLOB app_sources src/*.c)
target_sources(app PRIVATE ${app_sources})
target_include_directories(app PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/vectors)
add_dependencies(app mpipe_ipc_vectors_header)
