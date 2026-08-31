# Bracino bench ESP-NOW master — DN003 wire-contract test harness (issue 013)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Shared wire-law header (espnow_schema.h) — same canonical registry as node
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../shared")

project(bench-espnow-master)
