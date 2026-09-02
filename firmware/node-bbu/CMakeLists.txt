# Bracino node-bbu — bring-up (Q1 relay, A0–A3, NTC °C, params, DN003 client)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Shared wire-law header (espnow_schema.h) lives in ../shared — one
# canonical registry included by node AND bench-master firmware.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../shared")

# 012 System Data screen: UTC build stamp + build alias shown as firmware
# revision. The alias defaults to the git short hash of the exact tree
# being built (fallback "dev" outside git) — uniquely identifies what's on
# the device. Override per bench/field build:
#   idf.py -D'BRACINO_BUILD_NAME="b2"' ...   (or edit the default below)
execute_process(COMMAND date -u +%Y%m%d.%H%M
                OUTPUT_VARIABLE BRACINO_BUILD_STAMP
                OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} rev-parse --short HEAD
                OUTPUT_VARIABLE BRACINO_GIT_HASH
                OUTPUT_STRIP_TRAILING_WHITESPACE
                ERROR_QUIET)
if(NOT BRACINO_GIT_HASH)
    set(BRACINO_GIT_HASH "dev")
endif()
# mark dirty trees so a flashed bin is never confused with a pushed commit
execute_process(COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} diff --quiet
                RESULT_VARIABLE BRACINO_DIRTY
                OUTPUT_QUIET ERROR_QUIET)
if(NOT BRACINO_DIRTY EQUAL 0)
    set(BRACINO_GIT_HASH "${BRACINO_GIT_HASH}-m")
endif()
# Respect an explicit -D override (e.g. "b2"); otherwise always refresh to
# the current tree hash (old configure caches keep stale values otherwise).
if(BRACINO_BUILD_NAME STREQUAL "" OR BRACINO_BUILD_NAME STREQUAL "dev")
    set(BRACINO_BUILD_NAME "${BRACINO_GIT_HASH}" CACHE STRING
        "short build alias for the System Data screen" FORCE)
endif()
add_compile_definitions(BRACINO_BUILD="${BRACINO_BUILD_STAMP}"
                        BRACINO_BUILD_NAME="${BRACINO_BUILD_NAME}")

project(node-bbu)
