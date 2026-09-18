#
# Copyright 2026 NXP
#
# SPDX-License-Identifier: Apache-2.0
#

if(CONFIG_MPIPE_IPC)
  zephyr_library_named(mpipe_ipc)

  # Transport-independent: wire format, session handshake, bring-up barrier.
  # These build anywhere, which is what allows them to be unit tested on a host.
  zephyr_library_sources(mpipe_ipc_msg.c)
  zephyr_library_sources(mpipe_ipc_protocol.c)
  zephyr_library_sources(mpipe_ipc_session.c)
  zephyr_library_sources(mpipe_ipc_transport.c)

  # Pipeline elements that carry a pipeline between two cores. From the libMP
  # IPC plugin (Zephyr PR #114088), adapted to the mpipe element model.
  zephyr_library_sources_ifdef(CONFIG_MPIPE_IPC_PLUGIN mpipe_ipc_plugin_sink.c)
  zephyr_library_sources_ifdef(CONFIG_MPIPE_IPC_PLUGIN mpipe_ipc_plugin_src.c)

  # The only piece that needs a real backend.
  zephyr_library_sources_ifdef(CONFIG_IPC_SERVICE mpipe_ipc_backend.c)
endif()
