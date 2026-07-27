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
  URL https://github.com/NordicSemiconductor/nrfx/archive/refs/tags/v3.9.0.tar.gz
  SOURCE_SUBDIR cmake-noop
)
FetchContent_Declare(cmsis6
  URL https://github.com/ARM-software/CMSIS_6/archive/refs/tags/v6.1.0.tar.gz
  SOURCE_SUBDIR cmake-noop
)
FetchContent_Declare(freertos_kernel
  URL https://github.com/FreeRTOS/FreeRTOS-Kernel/archive/refs/tags/V11.1.0.tar.gz
)
# Crypto deps and the Monocypher rename contract are shared with the host
# test build via one fragment; see components/crypto/crypto_deps.cmake.
include(${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_deps.cmake)
bramble_crypto_deps_declare()
FetchContent_Declare(cjson
  URL https://github.com/DaveGamble/cJSON/archive/refs/tags/v1.7.18.tar.gz
  SOURCE_SUBDIR cmake-noop
)
FetchContent_MakeAvailable(nrfx cmsis6 mbedtls monocypher cjson)
bramble_crypto_deps_exclude_tls()
bramble_add_monocypher_ed25519(monocypher_ed25519)

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

# The portable Bramble stack: every component the seam map classifies as
# pure-C or shallow-coupled (shimmed), by explicit file list so the build is
# the inventory. Files needing real ESP peripherals do not belong here.
set(BRAMBLE_COMPONENTS ${CMAKE_CURRENT_SOURCE_DIR}/../components)
add_library(bramble_core STATIC
  ${BRAMBLE_COMPONENTS}/packet/packet.c
  ${BRAMBLE_COMPONENTS}/routing/routing.c
  ${BRAMBLE_COMPONENTS}/routing/discovery.c
  ${BRAMBLE_COMPONENTS}/routing/beacon.c
  ${BRAMBLE_COMPONENTS}/routing/forwarding.c
  ${BRAMBLE_COMPONENTS}/routing/channel_flood.c
  ${BRAMBLE_COMPONENTS}/routing_auth/routing_auth.c
  ${BRAMBLE_COMPONENTS}/dedup/dedup.c
  ${BRAMBLE_COMPONENTS}/dm_session/dm_session.c
  ${BRAMBLE_COMPONENTS}/fragment/fragment.c
  ${BRAMBLE_COMPONENTS}/replay_window/replay_window.c
  ${BRAMBLE_COMPONENTS}/replay_window/replay_deferred.c
  ${BRAMBLE_COMPONENTS}/security/security.c
  ${BRAMBLE_COMPONENTS}/security/dummy_traffic.c
  ${BRAMBLE_COMPONENTS}/timesync/timesync.c
  ${BRAMBLE_COMPONENTS}/airtime/airtime_budget.c
  ${BRAMBLE_COMPONENTS}/freq_plan/freq_plan.c
  ${BRAMBLE_COMPONENTS}/mailbox/mailbox.c
  ${BRAMBLE_COMPONENTS}/nonce_counter/nonce_counter.c
  ${BRAMBLE_COMPONENTS}/bramble_probe/bramble_probe.c
  ${BRAMBLE_COMPONENTS}/channel/channel_key.c
  ${BRAMBLE_COMPONENTS}/channel/channel_msg.c
  ${BRAMBLE_COMPONENTS}/channel/public_channel.c
  ${BRAMBLE_COMPONENTS}/channel/channel_storage.c
  ${BRAMBLE_COMPONENTS}/identity/identity.c
  ${BRAMBLE_COMPONENTS}/identity/identity_store.c
  ${BRAMBLE_COMPONENTS}/location/location.c
  ${BRAMBLE_COMPONENTS}/network_key/network_key.c
  ${BRAMBLE_COMPONENTS}/msg_store/msg_store.c
  ${BRAMBLE_COMPONENTS}/reliability/reliability.c
  ${BRAMBLE_COMPONENTS}/rpc/rpc_dispatcher.c
  ${BRAMBLE_COMPONENTS}/rpc/rpc_auth.c
  ${BRAMBLE_COMPONENTS}/traffic_debug/traffic_debug.c
  ${BRAMBLE_COMPONENTS}/ui/ui_manager.c
  ${BRAMBLE_COMPONENTS}/ui/ui_format.c
  ${BRAMBLE_COMPONENTS}/ui/sas_format.c
  ${BRAMBLE_COMPONENTS}/radio/radio_airtime.c
  ${BRAMBLE_COMPONENTS}/radio/radio_profiles.c
  ${BRAMBLE_COMPONENTS}/radio/tx_gate.c
  ${BRAMBLE_COMPONENTS}/gps/nmea_parser.c
  ${BRAMBLE_COMPONENTS}/battery/battery_pct.c
  ${BRAMBLE_COMPONENTS}/indicators/alerts.c
  ${cjson_SOURCE_DIR}/cJSON.c
)
target_include_directories(bramble_core PUBLIC ${cjson_SOURCE_DIR})
foreach(comp packet routing routing_auth dedup dm_session fragment replay_window
        security timesync airtime freq_plan mailbox nonce_counter bramble_probe
        channel identity location network_key msg_store reliability rpc
        traffic_debug ui radio gps battery indicators crypto nvs_keys
        board_config)
  target_include_directories(bramble_core PUBLIC ${BRAMBLE_COMPONENTS}/${comp}/include)
endforeach()
target_include_directories(bramble_core BEFORE PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/shim/include
  ${CMAKE_CURRENT_SOURCE_DIR}/boards
)
# radio_internal.h (the tx_gate-only transmit primitive) lives outside
# include/ on purpose; radio_null.c implements it.
target_include_directories(bramble_core PUBLIC ${BRAMBLE_COMPONENTS}/radio)
target_link_libraries(bramble_core PUBLIC freertos_kernel)

add_executable(bramble-nrf
  src/main_nrf.c
  src/app_init.c
  src/radio_null.c
  shim/console_uart.c
  shim/esp_timer_freertos.c
  shim/esp_random_nrf.c
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
  BRAMBLE_GIT_DESCRIBE="${BRAMBLE_GIT_DESCRIBE}"
)
target_link_libraries(bramble-nrf PRIVATE bramble_core nrfx_lib freertos_kernel mbedcrypto monocypher_ed25519)
# Flash layout: `swd` (default) links at 0x0 for the dev kit; `uf2` links at
# 0x26000 for the T1000-E's stock Adafruit UF2 bootloader and also emits
# bramble-nrf.uf2 for drag-and-drop flashing.
set(BRAMBLE_NRF_LAYOUT swd CACHE STRING "Flash layout: swd or uf2")
# App base under the T1000-E's Adafruit UF2 bootloader (must match
# ld/nrf52840_s140_app.ld FLASH ORIGIN).
set(BRAMBLE_UF2_BASE 0x26000)
if(BRAMBLE_NRF_LAYOUT STREQUAL "uf2")
  set(BRAMBLE_LD ${CMAKE_CURRENT_SOURCE_DIR}/ld/nrf52840_s140_app.ld)
else()
  set(BRAMBLE_LD ${nrfx_SOURCE_DIR}/mdk/nrf52840_xxaa.ld)
endif()
target_link_options(bramble-nrf PRIVATE
  -T${BRAMBLE_LD}
  -L${nrfx_SOURCE_DIR}/mdk
  -Wl,-Map=bramble-nrf.map
)
if(BRAMBLE_NRF_LAYOUT STREQUAL "uf2")
  add_custom_command(TARGET bramble-nrf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:bramble-nrf> bramble-nrf.bin
    COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/make_uf2.py bramble-nrf.bin
            --base ${BRAMBLE_UF2_BASE} -o bramble-nrf.uf2
    COMMENT "Packaging bramble-nrf.uf2 (base ${BRAMBLE_UF2_BASE}, family 0xADA52840)"
  )
endif()

add_custom_command(TARGET bramble-nrf POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:bramble-nrf> bramble-nrf.hex
  COMMAND python3 ${CMAKE_CURRENT_SOURCE_DIR}/scripts/size_report.py
          $<TARGET_FILE:bramble-nrf> --json size-report.json
  COMMENT "Generating bramble-nrf.hex and enforcing the RAM budget"
)
