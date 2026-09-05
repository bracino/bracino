idf_component_register(
    SRCS "main.c" "net.c" "softap.c" "amb.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash esp_wifi esp_event esp_netif esp_timer
             esp_http_server mqtt esp_adc bracino_schema
)
