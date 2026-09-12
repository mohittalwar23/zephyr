#
# Copyright (c) 2026 Mohit Talwar
#
# SPDX-License-Identifier: Apache-2.0
#

if(CONFIG_MPIPE_IPC)
  zephyr_library_named(mpipe_ipc)
  zephyr_library_sources(mpipe_ipc_protocol.c)
endif()
zephyr_library_sources(mpipe_ipc_session.c)
