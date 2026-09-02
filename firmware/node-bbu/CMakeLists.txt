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
add_custom_target(bracino_build_id
    COMMAND bash ${CMAKE_CURRENT_LIST_DIR}/gen_build_id.sh ${CMAKE_BINARY_DIR}
    COMMENT "bracino: refresh build id (git hash + UTC stamp)"
    VERBATIM)

project(node-bbu)

# build id must be fresh before anything compiles
add_dependencies(${CMAKE_PROJECT_NAME}.elf bracino_build_id)
