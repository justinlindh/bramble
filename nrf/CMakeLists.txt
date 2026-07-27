# Bramble nRF52840 target (Wio-WM1110 dev kit / SenseCAP T1000-E).
# Standalone bare-metal build: FreeRTOS + nrfx, no ESP-IDF, no SoftDevice.
# Configure with:
#   cmake -S nrf -B nrf/build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=MinSizeRel
cmake_minimum_required(VERSION 3.24)
project(bramble-nrf C ASM)

include(FetchContent)

# SOURCE_SUBDIR points at a directory without a CMakeLists.txt on purpose:
# these are source drops built by explicit file lists below, not
# add_subdirectory projects.
FetchContent_Declare(nrfx
  GIT_REPOSITORY https://github.com/NordicSemiconductor/nrfx.git
  GIT_TAG v3.9.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR cmake-noop
)
FetchContent_Declare(cmsis6
  GIT_REPOSITORY https://github.com/ARM-software/CMSIS_6.git
  GIT_TAG v6.1.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR cmake-noop
)
FetchContent_Declare(freertos_kernel
  GIT_REPOSITORY https://github.com/FreeRTOS/FreeRTOS-Kernel.git
  GIT_TAG V11.1.0
  GIT_SHALLOW TRUE
)
# Same pinned crypto stack the host nRF-backend test suites verify against
# (test/CMakeLists.txt); mbedtls builds with the minimal Bramble config.
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(MBEDTLS_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/config/mbedtls_config_bramble.h" CACHE STRING "" FORCE)
FetchContent_Declare(mbedtls
  URL https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-3.6.2/mbedtls-3.6.2.tar.bz2
)
FetchContent_Declare(monocypher
  URL https://github.com/LoupVaillant/Monocypher/archive/refs/tags/4.0.2.tar.gz
  SOURCE_SUBDIR cmake-noop
)
FetchContent_MakeAvailable(nrfx cmsis6 mbedtls monocypher)
# Only mbedcrypto is consumed; skip building the TLS and X.509 libraries.
set_target_properties(mbedtls mbedx509 PROPERTIES EXCLUDE_FROM_ALL TRUE)

# Monocypher's ed25519 unit exports names colliding with crypto.h; build it
# (and only it) under mono_ed25519_* renames. See
# components/crypto/ed25519_monocypher.h.
add_library(monocypher_ed25519 STATIC
  ${monocypher_SOURCE_DIR}/src/monocypher.c
  ${monocypher_SOURCE_DIR}/src/optional/monocypher-ed25519.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/ed25519_monocypher.c
)
target_include_directories(monocypher_ed25519 PUBLIC
  ${monocypher_SOURCE_DIR}/src
  ${monocypher_SOURCE_DIR}/src/optional
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_compile_definitions(monocypher_ed25519 PRIVATE
  BRAMBLE_PLATFORM_NRF
  crypto_ed25519_key_pair=mono_ed25519_key_pair
  crypto_ed25519_sign=mono_ed25519_sign
  crypto_ed25519_check=mono_ed25519_check
)

# FreeRTOS-Kernel's own CMake wants a freertos_config interface target
# providing FreeRTOSConfig.h.
add_library(freertos_config INTERFACE)
target_include_directories(freertos_config INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/config)
set(FREERTOS_PORT GCC_ARM_CM4F CACHE STRING "" FORCE)
set(FREERTOS_HEAP 4 CACHE STRING "" FORCE)
FetchContent_MakeAvailable(freertos_kernel)

# nrfx as an explicit-source static library. Sources are listed one by one on
# purpose: the enable set in config/nrfx_config.h and this list must agree.
add_library(nrfx_lib STATIC
  ${nrfx_SOURCE_DIR}/mdk/system_nrf52840.c
  ${nrfx_SOURCE_DIR}/mdk/gcc_startup_nrf52840.S
  ${nrfx_SOURCE_DIR}/drivers/src/nrfx_uarte.c
  ${nrfx_SOURCE_DIR}/drivers/src/nrfx_rng.c
  ${nrfx_SOURCE_DIR}/helpers/nrfx_flag32_allocator.c
)
target_include_directories(nrfx_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/config
  ${nrfx_SOURCE_DIR}
  ${nrfx_SOURCE_DIR}/drivers/include
  ${nrfx_SOURCE_DIR}/hal
  ${nrfx_SOURCE_DIR}/mdk
  ${nrfx_SOURCE_DIR}/soc
  ${nrfx_SOURCE_DIR}/templates
  ${cmsis6_SOURCE_DIR}/CMSIS/Core/Include
)
target_compile_definitions(nrfx_lib PUBLIC NRF52840_XXAA)

execute_process(
  COMMAND git describe --always --dirty
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
  OUTPUT_VARIABLE BRAMBLE_GIT_DESCRIBE
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)

add_executable(bramble-nrf
  src/main_nrf.c
  shim/console_uart.c
  shim/esp_timer_freertos.c
  shim/esp_random_nrf.c
  shim/esp_system_nrf.c
  shim/nvs_ram.c
  shim/mbedtls_platform_freertos.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_entropy.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_esp.c
)
target_include_directories(bramble-nrf PRIVATE
  boards
  shim/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_compile_definitions(bramble-nrf PRIVATE BRAMBLE_PLATFORM_NRF)
target_compile_definitions(bramble-nrf PRIVATE
  BRAMBLE_CONSOLE_UART=${BRAMBLE_CONSOLE_UART}
  BRAMBLE_GIT_DESCRIBE="${BRAMBLE_GIT_DESCRIBE}"
)
target_link_libraries(bramble-nrf PRIVATE nrfx_lib freertos_kernel mbedcrypto monocypher_ed25519)
target_link_options(bramble-nrf PRIVATE
  -T${nrfx_SOURCE_DIR}/mdk/nrf52840_xxaa.ld
  -L${nrfx_SOURCE_DIR}/mdk
  -Wl,-Map=bramble-nrf.map
)

add_custom_command(TARGET bramble-nrf POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:bramble-nrf> bramble-nrf.hex
  COMMAND ${CMAKE_SIZE} $<TARGET_FILE:bramble-nrf>
  COMMENT "Generating bramble-nrf.hex"
)
