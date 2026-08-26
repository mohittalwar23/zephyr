# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(dma_reload)

target_sources(app PRIVATE src/main.c src/test_sdma_irq.c
	${ZEPHYR_BASE}/drivers/dma/dma_nxp_sdma_accounting.c
	${ZEPHYR_BASE}/drivers/dma/dma_nxp_sdma_append.c
	${ZEPHYR_BASE}/drivers/dma/dma_nxp_sdma_irq.c
	${ZEPHYR_BASE}/drivers/dma/dma_nxp_sdma_lifecycle.c)
target_include_directories(app PRIVATE ${ZEPHYR_BASE}/drivers/dma)
