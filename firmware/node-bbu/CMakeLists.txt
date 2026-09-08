# Bracino node-bbu — bring-up (Q1 relay, A0–A3, NTC °C, params, DN003 client)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Shared wire-law header (espnow_schema.h) lives in ../shared — one
# canonical registry included by node AND bench-master firmware.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../shared")

# 012 System Data screen: UTC build stamp + git-hash alias as firmware
# revision. Generated per BUILD (not per configure) by gen_build_id.sh —
# a CMake-cached define let a stale alias reach a flashed bin (see the
# script header). main.c/ui.c include "bracino_build_id.h" from build/.
#
# 2026-09-08: wiring the generator as an ELF-only dependency let stale
# objects through — the header is rewritten just before LINK, so objects
# compiled in a prior build pass the staleness check and the relinked bin
# carries the previous build's identity (flashed in the field: bin said
# b36a268-m while the header said 69e43a8). Proper wiring: the header is
# a declared OUTPUT (so it has a real build rule), an ALL target keeps
# per-build refresh, and the consuming sources carry OBJECT_DEPENDS so
# they compile only after the header is fresh (depfile + explicit dep).
set(BRACINO_BUILD_ID_HDR "${CMAKE_BINARY_DIR}/bracino_build_id.h")
add_custom_command(
    OUTPUT "${BRACINO_BUILD_ID_HDR}"
    COMMAND bash ${CMAKE_CURRENT_LIST_DIR}/gen_build_id.sh ${CMAKE_BINARY_DIR}
    COMMENT "bracino: refresh build id (git hash + UTC stamp)"
    VERBATIM)
add_custom_target(bracino_build_id ALL DEPENDS "${BRACINO_BUILD_ID_HDR}")

project(node-bbu)
