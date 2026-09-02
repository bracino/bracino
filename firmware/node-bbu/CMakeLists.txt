# Bracino node-bbu — bring-up (Q1 relay, A0–A3, NTC °C, params, DN003 client)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Shared wire-law header (espnow_schema.h) lives in ../shared — one
# canonical registry included by node AND bench-master firmware.
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../shared")

# 012 System Data screen: UTC build stamp + build alias shown as firmware
# revision. The alias ALWAYS reflects the git short hash of the exact tree
# being built (+ "-m" if that dir has uncommitted changes) — deterministic,
# uniquely identifies what's on the device, no build-time ceremony. (A
# named-alias -D override was tried and dropped: CMake cache staleness made
# it a footgun. If you ever need a custom label, edit BRACINO_BUILD_NAME
# here or patch ui.c.)
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
# (scoped to firmware/ — unrelated in-flight hardware/doc churn must not
# change the flashed binary's identity)
execute_process(COMMAND git -C ${CMAKE_CURRENT_LIST_DIR} diff --quiet -- ${CMAKE_CURRENT_LIST_DIR}
                RESULT_VARIABLE BRACINO_DIRTY
                OUTPUT_QUIET ERROR_QUIET)
if(NOT BRACINO_DIRTY EQUAL 0)
    set(BRACINO_GIT_HASH "${BRACINO_GIT_HASH}-m")
endif()
set(BRACINO_BUILD_NAME "${BRACINO_GIT_HASH}" CACHE STRING
    "short build alias for the System Data screen" FORCE)
add_compile_definitions(BRACINO_BUILD="${BRACINO_BUILD_STAMP}"
                        BRACINO_BUILD_NAME="${BRACINO_BUILD_NAME}")

project(node-bbu)
