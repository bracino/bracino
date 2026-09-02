idf_component_register(
    SRCS "main.c" "ntc.c" "params.c" "control.c" "tft.c" "enc.c" "ui.c" "comms.c"
    INCLUDE_DIRS "." "${CMAKE_BINARY_DIR}"
    REQUIRES driver nvs_flash esp_lcd esp_timer esp_wifi esp_event
             esp_netif bracino_schema
)
