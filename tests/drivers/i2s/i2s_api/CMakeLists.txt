# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(i2s_api)

if(CONFIG_I2S_MCUX_SAI_HOST_SEAM_TEST)
  set(app_sources
    src/test_mcux_dma_integration.c
    src/test_mcux_dma_selection.c
    src/test_mcux_sai_stream.c
  )
else()
  file(GLOB app_sources src/*.c)
  list(FILTER app_sources EXCLUDE REGEX "/test_mcux_.*\\.c$")
  if(app_sources STREQUAL "")
    message(FATAL_ERROR "no test sources selected")
  endif()
endif()

target_sources(app PRIVATE ${app_sources})

if(CONFIG_I2S_MCUX_SAI_HOST_SEAM_TEST AND NOT CONFIG_DMA_NXP_SDMA)
  target_sources(app PRIVATE ${ZEPHYR_BASE}/drivers/dma/dma_nxp_sdma_common.c)
endif()

if(CONFIG_I2S_MCUX_SAI_HOST_SEAM_TEST AND NOT CONFIG_I2S_MCUX_SAI)
  target_sources(app PRIVATE ${ZEPHYR_BASE}/drivers/i2s/i2s_mcux_sai_stream.c)
endif()

if(CONFIG_SOC_MIMX8ML8_M7 AND CONFIG_DMA_NXP_SDMA)
  zephyr_linker_sources(RAM_SECTIONS boards/imx8mp-noncacheable.ld)
endif()
