idf_component_register(
    SRCS "main.c" "ntc.c" "params.c" "control.c"
    INCLUDE_DIRS "."
    REQUIRES driver nvs_flash
)
