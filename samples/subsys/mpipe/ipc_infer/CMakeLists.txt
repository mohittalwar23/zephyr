# Copyright 2026 NXP
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mpipe_ipc_infer)

target_sources(app PRIVATE src/main.c)

if(CONFIG_SOC_MIMX8ML8_ADSP)
  # Inference runs only on the DSP. Building TFLM into the M7 image as well
  # would cost it a megabyte it has no use for.
  set(NO_THREADSAFE_STATICS $<TARGET_PROPERTY:compiler-cpp,no_threadsafe_statics>)
  zephyr_compile_options($<$<COMPILE_LANGUAGE:CXX>:${NO_THREADSAFE_STATICS}>)

  set(SIGNAL_DIR ${ZEPHYR_TFLITE_MICRO_MODULE_DIR}/signal)

  # The model and its runner come from the micro_speech sample rather than a
  # copy of it. That sample is Zephyr PR #96657 and is not merged yet; until it
  # is, it has to be applied to the tree for this one to build.
  set(MICRO_SPEECH_DIR
      ${ZEPHYR_BASE}/samples/modules/tflite-micro/micro_speech/src/inference)

  if(NOT EXISTS ${MICRO_SPEECH_DIR}/model_runner.cpp)
    message(FATAL_ERROR
      "This sample needs the micro_speech sample from Zephyr PR #96657, which "
      "is not in this tree. Apply it, or fetch it with:\n"
      "  git fetch https://github.com/thong-phn/zephyr.git gsoc2025\n"
      "  git cherry-pick -x <its three commits>")
  endif()

  target_include_directories(app PRIVATE
    src
    ${MICRO_SPEECH_DIR}
    ${SIGNAL_DIR}
    ${SIGNAL_DIR}/micro/kernels/
    ${SIGNAL_DIR}/src
  )

  target_sources(app PRIVATE
    ${MICRO_SPEECH_DIR}/model_runner.cpp
    ${MICRO_SPEECH_DIR}/audio_preprocessor_int8_model.cpp
    ${MICRO_SPEECH_DIR}/micro_speech_quantized_model.cpp
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
else()
  target_sources(app PRIVATE src/pipeline.c)
endif()
