# Bracino field logger-gateway — issue 015 (DN003 wire law + DN004 shape)
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Shared wire-law header (espnow_schema.h) — same canonical registry as node
list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../shared")

# Build id (git hash + UTC stamp) into build/bracino_build_id.h, per BUILD.
# Same scheme as node-bbu 012 — see gen_build_id.sh header for why this is
# a build-time script and not a cached define.
add_custom_target(bracino_build_id
    COMMAND bash ${CMAKE_CURRENT_LIST_DIR}/gen_build_id.sh ${CMAKE_BINARY_DIR}
    COMMENT "bracino: refresh build id (git hash + UTC stamp)"
    VERBATIM)

project(gateway)

add_dependencies(${CMAKE_PROJECT_NAME}.elf bracino_build_id)
