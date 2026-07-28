idf_component_register(
    SRCS "crypto_esp.c" "crypto_entropy.c" "ed25519_sodium.c"
    INCLUDE_DIRS "include"
    REQUIRES mbedtls espressif__libsodium
)
