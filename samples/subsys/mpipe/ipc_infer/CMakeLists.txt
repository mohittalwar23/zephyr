# Copyright 2026 NXP
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mpipe_ipc_infer)

target_sources(app PRIVATE src/main.c)

if(CONFIG_SAMPLE_IPC_INFER_LIVE_CAPTURE)
  target_sources(app PRIVATE src/pipeline.c)
endif()

if(CONFIG_SOC_MIMX8ML8_ADSP)
  # Inference runs only on the DSP. Building TFLM into the M7 image as well
  # would cost it a megabyte it has no use for.
  set(NO_THREADSAFE_STATICS $<TARGET_PROPERTY:compiler-cpp,no_threadsafe_statics>)
  zephyr_compile_options($<$<COMPILE_LANGUAGE:CXX>:${NO_THREADSAFE_STATICS}>)

  set(SIGNAL_DIR ${ZEPHYR_TFLITE_MICRO_MODULE_DIR}/signal)

  target_include_directories(app PRIVATE
    src
    ${SIGNAL_DIR}
    ${SIGNAL_DIR}/micro/kernels/
    ${SIGNAL_DIR}/src
  )

  file(GLOB inference_sources src/inference/*)
  target_sources(app PRIVATE
    ${inference_sources}
    src/consumer.c
    src/infer_sink.c

    ${SIGNAL_DIR}/src/fft_auto_scale.cc
    ${SIGNAL_DIR}/src/max_abs.cc
    ${SIGNAL_DIR}/src/msb_32.cc
    ${SIGNAL_DIR}/src/energy.cc
    ${SIGNAL_DIR}/src/filter_bank.cc
    ${SIGNAL_DIR}/src/pcan_argc_fixed.cc
    ${SIGNAL_DIR}/src/filter_bank_log.cc
    ${SIGNAL_DIR}/src/log.cc
    ${SIGNAL_DIR}/src/filter_bank_spectral_subtraction.cc

    ${SIGNAL_DIR}/micro/kernels/energy.cc
    ${SIGNAL_DIR}/micro/kernels/fft_auto_scale_common.cc
    ${SIGNAL_DIR}/micro/kernels/filter_bank_square_root_common.cc
    ${SIGNAL_DIR}/micro/kernels/filter_bank_spectral_subtraction.cc
    ${SIGNAL_DIR}/micro/kernels/filter_bank.cc
    ${SIGNAL_DIR}/micro/kernels/pcan.cc
    ${SIGNAL_DIR}/micro/kernels/filter_bank_log.cc

    ${SIGNAL_DIR}/micro/kernels/xtensa/fft_auto_scale_kernel.cc
    ${SIGNAL_DIR}/micro/kernels/xtensa/filter_bank_square_root.cc
    ${SIGNAL_DIR}/micro/kernels/xtensa/xtensa_square_root.S
  )
elseif(NOT CONFIG_SAMPLE_IPC_INFER_LIVE_CAPTURE)
  target_sources(app PRIVATE src/test_clips.c)
endif()
