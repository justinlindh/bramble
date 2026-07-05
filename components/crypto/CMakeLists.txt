idf_component_register(
    SRCS "crypto_esp.c" "crypto_entropy.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls espressif__libsodium
)
