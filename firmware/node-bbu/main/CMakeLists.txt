idf_component_register(
    SRCS "main.c" "ntc.c" "params.c" "control.c" "tft.c" "enc.c" "ui.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash
)
