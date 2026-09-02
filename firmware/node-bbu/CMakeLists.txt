# Bracino node-bbu — bring-up (Q1 relay, A0–A3, NTC °C, params, DN003 client)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Shared wire-law header (espnow_schema.h) lives in ../shared — one
# canonical registry included by node AND bench-master firmware.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../shared")

# 012 System Data screen: UTC build stamp shown as firmware revision.
execute_process(COMMAND date -u +%Y%m%d.%H%M
                OUTPUT_VARIABLE BRACINO_BUILD_STAMP
                OUTPUT_STRIP_TRAILING_WHITESPACE)
add_compile_definitions(BRACINO_BUILD="${BRACINO_BUILD_STAMP}")

project(node-bbu)
