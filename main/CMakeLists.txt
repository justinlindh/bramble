idf_component_register(
    SRCS "main.c" "mesh_task.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event driver esp_timer
             display button ui crypto identity
             radio packet routing dedup security freq_plan
)
