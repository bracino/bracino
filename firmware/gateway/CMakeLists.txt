# Bracino field logger-gateway — issue 015 (DN003 wire law + DN004 shape)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Shared wire-law header (espnow_schema.h) — same canonical registry as node
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../shared")

# Build id (git hash + UTC stamp) into build/bracino_build_id.h, per BUILD.
# Same scheme as node-bbu 012 — see gen_build_id.sh header for why this is
# a build-time script and not a cached define.
#
# 2026-09-08: an ELF-only dependency let stale objects through — the
# header is rewritten just before LINK, so objects compiled in a prior
# build pass the staleness check and the relinked bin carries the
# previous build's identity (node flash shipped exactly that: bin said
# b36a268-m while the header said 69e43a8). Header is now a declared
# OUTPUT + ALL target, and consuming sources carry OBJECT_DEPENDS.
set(BRACINO_BUILD_ID_HDR "${CMAKE_BINARY_DIR}/bracino_build_id.h")
add_custom_command(
    OUTPUT "${BRACINO_BUILD_ID_HDR}"
    COMMAND bash ${CMAKE_CURRENT_LIST_DIR}/gen_build_id.sh ${CMAKE_BINARY_DIR}
    COMMENT "bracino: refresh build id (git hash + UTC stamp)"
    VERBATIM)
add_custom_target(bracino_build_id ALL DEPENDS "${BRACINO_BUILD_ID_HDR}")

project(gateway)
