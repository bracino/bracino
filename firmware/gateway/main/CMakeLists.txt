idf_component_register(
    SRCS "main.c" "net.c" "softap.c" "amb.c"
    INCLUDE_DIRS "." "${CMAKE_BINARY_DIR}"
    REQUIRES driver nvs_flash esp_wifi esp_event esp_netif esp_timer
             esp_http_server mqtt esp_adc bracino_schema
)

# Sources embedding the build id must compile after the generated header
# is refreshed (see top-level CMakeLists for the 2026-09-08 staleness fix).
set_source_files_properties(main.c softap.c PROPERTIES
    OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/bracino_build_id.h")
