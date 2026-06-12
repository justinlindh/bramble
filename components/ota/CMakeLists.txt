idf_component_register(
    SRCS "ota.c" "ota_url.c" "ota_version.c" "ota_origin.c" "ota_rollback.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_https_ota esp_http_client app_update nvs_flash nvs_keys
    PRIV_REQUIRES mbedtls
)
