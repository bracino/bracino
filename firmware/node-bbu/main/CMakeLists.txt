idf_component_register(
    SRCS "main.c" "ntc.c" "params.c" "control.c" "tft.c" "enc.c" "ui.c" "comms.c"
    INCLUDE_DIRS "." "${CMAKE_BINARY_DIR}"
    REQUIRES driver nvs_flash esp_lcd esp_timer esp_wifi esp_event
             esp_netif bracino_schema
)

# Sources embedding the build id must compile after the generated header
# is refreshed (see top-level CMakeLists for the 2026-09-08 staleness fix).
set_source_files_properties(main.c ui.c PROPERTIES
    OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/bracino_build_id.h")
