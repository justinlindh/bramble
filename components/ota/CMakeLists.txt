# esp_https_ota / app_update do not exist on the POSIX/Linux simulator: only
# the pure sources (URL/version/origin logic, all NVS- or string-based) build
# there. The device-only entry points (ota.c, ota_rollback.c) come from the
# emulator's driver component as "not supported" stubs.
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(
        SRCS "ota_url.c" "ota_version.c" "ota_rollback_policy.c" "ota_origin.c" "ota_progress.c"
        INCLUDE_DIRS "include"
        REQUIRES nvs_flash nvs_keys
    )
else()
    idf_component_register(
        SRCS "ota.c" "ota_url.c" "ota_version.c" "ota_origin.c" "ota_rollback.c"
             "ota_rollback_policy.c" "ota_progress.c"
        INCLUDE_DIRS "include"
        REQUIRES esp_https_ota esp_http_client app_update nvs_flash nvs_keys
        PRIV_REQUIRES mbedtls
    )
endif()
