# Single source of truth for the non-IDF crypto dependency pins and the
# Monocypher symbol-rename contract, shared by the nRF52840 target build
# (nrf/CMakeLists.txt) and the host test build (test/CMakeLists.txt). Keeping
# one copy is what makes "the host proves wire-compat for the exact library
# the device ships" true by construction.
#
# Usage:
#   include(.../components/crypto/crypto_deps.cmake)
#   bramble_crypto_deps_declare()          # before other FetchContent declares
#   FetchContent_MakeAvailable(... mbedtls monocypher ...)
#   bramble_crypto_deps_exclude_tls()      # after MakeAvailable
#   bramble_add_monocypher_ed25519(<lib>)  # the renamed provider library

include(FetchContent)

set(BRAMBLE_CRYPTO_DEPS_DIR ${CMAKE_CURRENT_LIST_DIR})

macro(bramble_crypto_deps_declare)
  set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
  set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
  # mbedtls's own -Werror trips new-GCC warnings in its TLS code; we only
  # consume mbedcrypto.
  set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
  # Both builds compile the device's minimal config, so the host suites
  # exercise the exact feature set the target ships (and build a fraction of
  # the default-config TU count).
  set(MBEDTLS_CONFIG_FILE "${BRAMBLE_CRYPTO_DEPS_DIR}/mbedtls_config_bramble.h" CACHE STRING "" FORCE)
  FetchContent_Declare(mbedtls
    URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
  )
  FetchContent_Declare(monocypher
    URL https://github.com/LoupVaillant/Monocypher/archive/refs/tags/4.0.2.tar.gz
    SOURCE_SUBDIR cmake-noop
  )
endmacro()

macro(bramble_crypto_deps_exclude_tls)
  # Only mbedcrypto is consumed; skip building the TLS and X.509 libraries.
  set_target_properties(mbedtls mbedx509 PROPERTIES EXCLUDE_FROM_ALL TRUE)
endmacro()

# Monocypher's ed25519 unit exports crypto_ed25519_sign/check/key_pair, which
# collide with Bramble's crypto.h names at link time, so its objects (and the
# provider TU, the only file that sees its header) compile under
# mono_ed25519_* renames. See ed25519_monocypher.c.
function(bramble_add_monocypher_ed25519 name)
  add_library(${name} STATIC
    ${monocypher_SOURCE_DIR}/src/monocypher.c
    ${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c
    ${BRAMBLE_CRYPTO_DEPS_DIR}/ed25519_monocypher.c
  )
  target_include_directories(${name} PUBLIC
    ${monocypher_SOURCE_DIR}/src
    ${monocypher_SOURCE_DIR}/src/optional
    ${BRAMBLE_CRYPTO_DEPS_DIR}/include
  )
  target_compile_definitions(${name} PRIVATE
    crypto_ed25519_key_pair=mono_ed25519_key_pair
    crypto_ed25519_sign=mono_ed25519_sign
    crypto_ed25519_check=mono_ed25519_check
  )
endfunction()
