#
# Copyright 2026 NXP
#
# SPDX-License-Identifier: Apache-2.0
#

if(CONFIG_MPIPE_IPC)
  zephyr_library_named(mpipe_ipc)

  # Transport-independent: wire format, session handshake, bring-up barrier.
  # These build anywhere, which is what allows them to be unit tested on a host.
  zephyr_library_sources(mpipe_ipc_protocol.c)
  zephyr_library_sources(mpipe_ipc_session.c)
  zephyr_library_sources(mpipe_ipc_transport.c)
  zephyr_library_sources(mpipe_ipc_ring.c)

  # The only piece that needs a real backend.
  zephyr_library_sources_ifdef(CONFIG_IPC_SERVICE mpipe_ipc_backend.c)
endif()
