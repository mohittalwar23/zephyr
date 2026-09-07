# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(imx8m_audio_clock)

target_sources(app PRIVATE src/main.c)

if(CONFIG_IMX8M_M7_AUDIO_PLL1)
  set(audio_clock_owner owned)
else()
  set(audio_clock_owner external)
endif()

# The expected AUDIOMIX attachments follow the active devicetree nodes, so the
# same verifier covers the combined, SAI-only, and PDM-only configurations.
set(audio_clock_attachments "")
foreach(node sai3 micfil)
  dt_nodelabel(node_path NODELABEL "${node}")
  if(node_path)
    dt_node_has_status(node_okay PATH "${node_path}" STATUS okay)
    if(node_okay)
      list(APPEND audio_clock_attachments "${node}")
    endif()
  endif()
endforeach()
list(JOIN audio_clock_attachments "," audio_clock_attach_arg)
