#Copyright (c) 2026 Qualcomm Innovation Center, Inc. All rights reserved.
#SPDX-License-Identifier: Apache-2.0

if(CONFIG_MP_IPC)
  zephyr_library_sources(
    mp_ipc_sink.c
    mp_ipc_src.c
    mp_ipc_translation.c
  )
endif()
