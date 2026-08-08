# Bramble nRF52840 target (Wio-WM1110 dev kit / SenseCAP T1000-E).
# Standalone bare-metal build: FreeRTOS + nrfx, no ESP-IDF, no SoftDevice.
# Configure with:
#   cmake -S nrf -B nrf/build -G Ninja \
#     -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=MinSizeRel
cmake_minimum_required(VERSION 3.24)
project(bramble-nrf C ASM)

# Toolchain identity, announced loudly at configure time.
#
# The RAM budget gate below fails the build on a byte count, and that count is a
# property of the compiler as much as of the source: at the margins this target
# runs at (single-digit bytes of headroom on the T1000-E static gate) a different
# GCC is a different measurement. `.arm-gcc-version` at the repo root is the
# single source of truth, CI asserts its runner image matches it, and a local
# build that does not match cannot be compared to a CI number. That is a warning
# rather than an error because a developer cannot always install an exact GCC
# from their distro; the fix is the pinned Arm release named in nrf/README.md,
# and until then every size number this build prints carries the mismatch with
# it (scripts/size_report.py stamps the toolchain into its output and JSON).
set(BRAMBLE_ARM_GCC_PIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../.arm-gcc-version")
file(READ "${BRAMBLE_ARM_GCC_PIN_FILE}" BRAMBLE_ARM_GCC_PIN)
string(STRIP "${BRAMBLE_ARM_GCC_PIN}" BRAMBLE_ARM_GCC_PIN)
message(STATUS "bramble-nrf toolchain: arm-none-eabi-gcc ${CMAKE_C_COMPILER_VERSION} (${CMAKE_C_COMPILER})")
if(CMAKE_C_COMPILER_VERSION VERSION_EQUAL "${BRAMBLE_ARM_GCC_PIN}")
  message(STATUS "bramble-nrf toolchain: matches the pin in .arm-gcc-version (${BRAMBLE_ARM_GCC_PIN}); size numbers are comparable with CI")
else()
  message(WARNING
    "bramble-nrf toolchain MISMATCH: this build uses arm-none-eabi-gcc "
    "${CMAKE_C_COMPILER_VERSION} but .arm-gcc-version pins ${BRAMBLE_ARM_GCC_PIN}, "
    "which is what CI builds with. Code size differs between GCC versions, so the "
    "RAM budget gate can pass here and fail in CI (or the reverse). Do not report "
    "these size numbers as CI numbers. See nrf/README.md for the pinned toolchain "
    "download.")
endif()

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
# Semtech LR11xx driver (Clear BSD). v3.0.0; do not pin below v2.7.0 (an
# lr11xx_hal_read pointer-conversion fix lives there).
# Apache NimBLE, host + controller, no SoftDevice. Two patches are required.
# The ISR-safe critical section, because upstream's uses vPortEnterCritical,
# which the Cortex-M FreeRTOS port asserts on when the link layer calls it
# from its RADIO/RTC0 ISRs. And the duplicate-PDU tolerance during encryption
# start, without which a retransmitted plaintext control PDU makes the
# controller terminate the link with 0x3D; see that patch for the full story.
set(NIMBLE_PATCHES
  ${CMAKE_CURRENT_SOURCE_DIR}/patches/nimble-isr-safe-critical.patch
  ${CMAKE_CURRENT_SOURCE_DIR}/patches/nimble-dup-pdu-during-enc-start.patch)
FetchContent_Declare(nimble
  GIT_REPOSITORY https://github.com/apache/mynewt-nimble.git
  GIT_TAG nimble_1_10_0_tag
  GIT_SHALLOW TRUE
  # Idempotent but not silent: if the patch is already applied (CMake re-runs
  # this step on every reconfigure) the reverse-check short-circuits;
  # otherwise it must apply cleanly or the build fails. Never `|| true`: a
  # patch that quietly fails to apply produces a build that asserts inside
  # the link layer's first interrupt, which is miserable to debug.
  #
  # One list, used for both the check and the apply: git apply is
  # all-or-nothing across the files it is given, so the set must be identical
  # on both sides. Naming it once is what keeps them that way; adding a patch
  # to only one list would give a reverse-check that never short-circuits and
  # therefore a re-apply failure on every reconfigure.
  PATCH_COMMAND git apply --reverse --check --ignore-whitespace ${NIMBLE_PATCHES} ||
                git apply --ignore-whitespace ${NIMBLE_PATCHES}
)
FetchContent_Declare(littlefs
  URL https://github.com/littlefs-project/littlefs/archive/refs/tags/v2.11.3.tar.gz
  SOURCE_SUBDIR cmake-noop
)
FetchContent_Declare(swdr001
  URL https://github.com/Lora-net/SWDR001/archive/refs/tags/v3.0.0.tar.gz
  SOURCE_SUBDIR cmake-noop
)
FetchContent_MakeAvailable(nrfx cmsis6 mbedtls monocypher cjson swdr001 littlefs nimble)
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
  ${nrfx_SOURCE_DIR}/drivers/src/nrfx_spim.c
  ${nrfx_SOURCE_DIR}/drivers/src/nrfx_nvmc.c
  ${nrfx_SOURCE_DIR}/drivers/src/nrfx_gpiote.c
  ${nrfx_SOURCE_DIR}/drivers/src/nrfx_saadc.c
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
# MSP stack and newlib heap, tuned from the nrfx 16K defaults: main() only
# bootstraps, ISRs are shallow, and newlib allocates little (printf buffers);
# the mesh runs on FreeRTOS task stacks and ucHeap. Sized when the mesh
# statics first pushed the RAM gate over budget.
# __HEAP_SIZE=0: newlib's sbrk heap is dead weight now that
# shim/malloc_freertos.c serves malloc from the FreeRTOS pool, and reserving
# a second region would just take RAM away from the one heap that is used.
target_compile_definitions(nrfx_lib PRIVATE __STACK_SIZE=8192 __HEAP_SIZE=0)

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
  ${BRAMBLE_COMPONENTS}/msg_store/msg_store_lfs.c
  ${BRAMBLE_COMPONENTS}/reliability/reliability.c
  ${BRAMBLE_COMPONENTS}/rpc/rpc_dispatcher.c
  ${BRAMBLE_COMPONENTS}/rpc/rpc_auth.c
  ${BRAMBLE_COMPONENTS}/traffic_debug/traffic_debug.c
  ${BRAMBLE_COMPONENTS}/ui/ui_manager.c
  ${BRAMBLE_COMPONENTS}/ui/ui_format.c
  ${BRAMBLE_COMPONENTS}/ui/sas_format.c
  ${BRAMBLE_COMPONENTS}/ui/gnss_status.c
  ${BRAMBLE_COMPONENTS}/radio/radio_airtime.c
  ${BRAMBLE_COMPONENTS}/radio/radio_profiles.c
  ${BRAMBLE_COMPONENTS}/radio/tx_gate.c
  ${BRAMBLE_COMPONENTS}/radio/tx_gate_esp.c
  ${BRAMBLE_COMPONENTS}/radio/phy_passthrough.c
  ${BRAMBLE_COMPONENTS}/timezone/bramble_tz.c
  ${BRAMBLE_COMPONENTS}/timezone/tz_store.c
  ${BRAMBLE_COMPONENTS}/gps/nmea_parser.c
  ${BRAMBLE_COMPONENTS}/gps/gps_pref.c
  ${BRAMBLE_COMPONENTS}/gps/gps_feed.c
  ${BRAMBLE_COMPONENTS}/gps/gps_duty.c
  ${BRAMBLE_COMPONENTS}/battery/battery_pct.c
  ${BRAMBLE_COMPONENTS}/battery/battery_helpers.c
  ${BRAMBLE_COMPONENTS}/battery/battery_wrappers.c
  ${BRAMBLE_COMPONENTS}/indicators/alerts.c
  ${BRAMBLE_COMPONENTS}/ota/ota_progress.c
  ${BRAMBLE_COMPONENTS}/ota/ota_origin.c
  ${BRAMBLE_COMPONENTS}/ota/ota_url.c
  ${BRAMBLE_COMPONENTS}/ota/ota_version.c
  ${BRAMBLE_COMPONENTS}/ota/ota_rollback_policy.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/mesh_task.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/mesh_beacon.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/mesh_persist.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/mesh_dm.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/mesh_reliability.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/mesh_probe.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/mesh_location.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/mesh_routing.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/mesh_channels.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/mesh_mailbox.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/util.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/rreq_pseudonym.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/beacon_policy_calc.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/probe_results.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/probe_reply.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/rerr_ack_fastfail.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/broadcast_delivery_receipt.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/delivery_event_ring.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/board.c
  ${cjson_SOURCE_DIR}/cJSON.c
)
target_include_directories(bramble_core PUBLIC ${cjson_SOURCE_DIR})
foreach(comp packet routing routing_auth dedup dm_session fragment replay_window
        security timesync airtime freq_plan mailbox nonce_counter bramble_probe
        channel identity location network_key msg_store reliability rpc
        traffic_debug ui radio gps battery indicators crypto nvs_keys ota
        board_config timezone)
  target_include_directories(bramble_core PUBLIC ${BRAMBLE_COMPONENTS}/${comp}/include)
endforeach()
target_include_directories(bramble_core BEFORE PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/shim/include
  ${CMAKE_CURRENT_SOURCE_DIR}/boards
)
# radio_internal.h (the tx_gate-only transmit primitive) lives outside
# include/ on purpose; radio_lr1110.c implements it.
target_include_directories(bramble_core PUBLIC ${BRAMBLE_COMPONENTS}/radio)
target_include_directories(bramble_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../main
  ${CMAKE_CURRENT_SOURCE_DIR}/src)
# PUBLIC so every translation unit that sees delivery_event_ring.h agrees on
# the struct size. 512 records (28KB) is an ESP-with-PSRAM figure; this chip
# has 256KB in total and no PSRAM to spill into, and the ring holds local
# delivery history for the UI rather than any protocol state, so depth here
# costs only how far back the timeline goes. 128 keeps a useful window and
# returns 21KB to the heap that pairing needs for its P-256 keypair.
target_compile_definitions(bramble_core PUBLIC BRAMBLE_PLATFORM_NRF
                                               DELIVERY_EVENT_RING_CAPACITY=128u)
target_link_libraries(bramble_core PUBLIC freertos_kernel littlefs_lib)

add_library(littlefs_lib STATIC
  ${littlefs_SOURCE_DIR}/lfs.c
  ${littlefs_SOURCE_DIR}/lfs_util.c
)
target_include_directories(littlefs_lib PUBLIC ${littlefs_SOURCE_DIR})
# The target has no stdio; littlefs's debug macros would pull printf in.
target_compile_definitions(littlefs_lib PUBLIC LFS_NO_DEBUG LFS_NO_WARN LFS_NO_ERROR
                                               LFS_NO_ASSERT LFS_NO_MALLOC)

# SWDR001: only the command areas the radio backend uses; the HAL under
# nrf/src implements its 6-function contract on nrfx SPIM2.
add_library(swdr001_lib STATIC
  ${swdr001_SOURCE_DIR}/src/lr11xx_system.c
  ${swdr001_SOURCE_DIR}/src/lr11xx_radio.c
  ${swdr001_SOURCE_DIR}/src/lr11xx_regmem.c
)
target_include_directories(swdr001_lib PUBLIC ${swdr001_SOURCE_DIR}/src)

# Settings/message filesystem partition. It sits above the application in
# both layouts so one base serves both: under uf2 the app region is
# 0x27000-0xC0000 and the stock bootloader owns 0xED000 up; under swd the
# app starts at 0x0 and the top of flash is ours.
set(BRAMBLE_LFS_BASE 0xC0000)
set(BRAMBLE_LFS_SIZE 0x2D000) # 180KB, ends at 0xED000


# NimBLE: sources hand-listed because the upstream porting Makefiles still
# reference paths that moved (nimble/drivers/nrf52 is now nrf5x, and the RAM
# transport was replaced by the unified transport).
file(GLOB NIMBLE_HOST_SRC ${nimble_SOURCE_DIR}/nimble/host/src/*.c)
file(GLOB NIMBLE_CTRL_SRC ${nimble_SOURCE_DIR}/nimble/controller/src/*.c)
file(GLOB NIMBLE_PORTING_SRC ${nimble_SOURCE_DIR}/porting/nimble/src/*.c)
add_library(nimble_lib STATIC
  ${NIMBLE_HOST_SRC}
  ${NIMBLE_CTRL_SRC}
  ${NIMBLE_PORTING_SRC}
  ${nimble_SOURCE_DIR}/nimble/host/services/gap/src/ble_svc_gap.c
  ${nimble_SOURCE_DIR}/nimble/host/services/gatt/src/ble_svc_gatt.c
  ${nimble_SOURCE_DIR}/nimble/host/store/config/src/ble_store_config.c
  ${nimble_SOURCE_DIR}/nimble/host/util/src/addr.c
  ${nimble_SOURCE_DIR}/nimble/transport/src/transport.c
  ${nimble_SOURCE_DIR}/nimble/drivers/nrf5x/src/ble_hw.c
  ${nimble_SOURCE_DIR}/nimble/drivers/nrf5x/src/ble_phy.c
  ${nimble_SOURCE_DIR}/nimble/drivers/nrf5x/src/nrf52/phy.c
  ${nimble_SOURCE_DIR}/nimble/drivers/nrf5x/src/ble_phy_trace.c
  ${nimble_SOURCE_DIR}/porting/npl/freertos/src/npl_os_freertos.c
)
target_include_directories(nimble_lib PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/config/nimble
  ${nimble_SOURCE_DIR}/nimble/include
  ${nimble_SOURCE_DIR}/nimble/host/include
  ${nimble_SOURCE_DIR}/nimble/host/services/gap/include
  ${nimble_SOURCE_DIR}/nimble/host/services/gatt/include
  ${nimble_SOURCE_DIR}/nimble/host/store/config/include
  # src/ too: ble_store_nvs.c implements the persistence hooks declared in
  # the store's private header, so both sides share one declaration.
  ${nimble_SOURCE_DIR}/nimble/host/store/config/src
  ${nimble_SOURCE_DIR}/nimble/host/util/include
  ${nimble_SOURCE_DIR}/nimble/controller/include
  ${nimble_SOURCE_DIR}/nimble/drivers/nrf5x/include
  ${nimble_SOURCE_DIR}/nimble/drivers/nrf5x/src
  ${nimble_SOURCE_DIR}/nimble/transport/include
  ${nimble_SOURCE_DIR}/porting/nimble/include
  ${nimble_SOURCE_DIR}/porting/npl/freertos/include
)
# NIMBLE_CFG_CONTROLLER selects the combined host+controller build. It is a
# command-line flag in the upstream Makefiles (which this build does not use,
# because they still reference moved paths), and without it nimble_port_init
# silently skips ble_ll_init and the controller is simply absent.
#
# FREERTOS makes the link layer place its RADIO interrupt at priority 5
# instead of 0. Priority 0 is numerically above configMAX_SYSCALL_INTERRUPT_
# PRIORITY, so the ISR's FromISR calls trip FreeRTOS's interrupt-priority
# assert; upstream already accounts for this, the flag just has to be set.
target_compile_definitions(nimble_lib PUBLIC NIMBLE_CFG_CONTROLLER=1 FREERTOS=1)
target_link_libraries(nimble_lib PUBLIC nrfx_lib freertos_kernel)
# Third-party code is not held to this repo's warning wall.
target_compile_options(nimble_lib PRIVATE -Wno-unused-parameter -Wno-sign-compare
                                          -Wno-implicit-fallthrough -Wno-unused-but-set-variable
                                          -include ${CMAKE_CURRENT_SOURCE_DIR}/config/nimble/bramble_nimble_decls.h)
target_link_libraries(nimble_lib PUBLIC mbedcrypto)

# Which physical board's pin map and profile to compile. The dev kit is the
# default; t1000e selects the SenseCAP card (nrf/boards/t1000e.h, consoleless,
# four pin RF switch) and its shared identity profile. Selected here, above
# add_executable, because the GPS source list below depends on it.
set(BRAMBLE_NRF_BOARD wm1110_devkit CACHE STRING "Board: wm1110_devkit or t1000e")
if(BRAMBLE_NRF_BOARD STREQUAL "t1000e")
  target_compile_definitions(bramble_core PUBLIC BRAMBLE_BOARD_T1000E CONFIG_BRAMBLE_BOARD_T1000E)
else()
  target_compile_definitions(bramble_core PUBLIC CONFIG_BRAMBLE_BOARD_WM1110)
endif()

# GNSS driver: the T1000-E gets the real AG3335 driver over UARTE1 plus the
# RPC event emitter; every other board (no exposed GNSS UART) gets the null
# backend.
if(BRAMBLE_NRF_BOARD STREQUAL "t1000e")
  set(BRAMBLE_NRF_GPS_SRCS shim/gps_t1000e.c src/gps_events.c)
else()
  set(BRAMBLE_NRF_GPS_SRCS shim/gps_null.c)
endif()

# Battery driver: the T1000-E gets the gated SAADC voltage read (drive the
# P1.06 sensor rail per read) plus CHRG/VBUS charge detect, all in
# shim/battery_t1000e.c; the dev kit (no battery sense wired) gets the null
# backend. Exactly one backend is compiled per board, BY FILENAME, so a
# wrong name here selects silently; the compile-time guard on
# BOARD_BATTERY_NEEDS_RAIL_GATE (nrf/boards/t1000e.h, checked by
# battery_null.c) is what turns a wrong selection into a build error.
if(BRAMBLE_NRF_BOARD STREQUAL "t1000e")
  set(BRAMBLE_NRF_BATTERY_SRCS shim/battery_t1000e.c)
else()
  set(BRAMBLE_NRF_BATTERY_SRCS shim/battery_null.c)
endif()

add_executable(bramble-nrf
  src/main_nrf.c
  src/app_init.c
  src/boot_trace.c
  src/boot_trace_scan.c
  src/radio_lr1110.c
  src/lr11xx_hal_nrf.c
  src/lfs_nvmc.c
  src/nimble_glue.c
  src/ble_store_nvs.c
  # Replaces upstream's porting/npl/freertos/src/nimble_port_freertos.c,
  # which is deliberately absent from nimble_lib above: the BLE task
  # priorities and stacks are this port's to choose.
  src/nimble_port_freertos.c
  src/ble_host.c
  ${BRAMBLE_COMPONENTS}/ble/ble_server.c
  ${BRAMBLE_COMPONENTS}/ble/ble_pairing_policy.c
  ${BRAMBLE_COMPONENTS}/ble/ble_pairing_store.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_token.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
  src/nrf_provision.c
  shim/console_uart.c
  shim/esp_timer_freertos.c
  shim/esp_random_nrf.c
  shim/esp_system_nrf.c
  ${BRAMBLE_NRF_GPS_SRCS}
  ${BRAMBLE_NRF_BATTERY_SRCS}
  shim/esp_app_desc_nrf.c
  shim/rpc_platform_stubs.c
  shim/nvs_lfs.c
  shim/mbedtls_platform_freertos.c
  shim/malloc_freertos.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_entropy.c
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_esp.c
)
target_include_directories(bramble-nrf PRIVATE
  boards
  src
  ${BRAMBLE_COMPONENTS}/ble/include
  ${BRAMBLE_COMPONENTS}/ota/include
  ${BRAMBLE_COMPONENTS}/display/include
  ${BRAMBLE_COMPONENTS}/wifi/include
  shim/include
  ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_compile_definitions(bramble-nrf PRIVATE BRAMBLE_PLATFORM_NRF)
target_compile_definitions(bramble-nrf PRIVATE
  BRAMBLE_GIT_DESCRIBE="${BRAMBLE_GIT_DESCRIBE}"
  BRAMBLE_LFS_BASE=${BRAMBLE_LFS_BASE}
  BRAMBLE_LFS_SIZE=${BRAMBLE_LFS_SIZE}
  $<$<BOOL:${BRAMBLE_LOG_DEBUG}>:BRAMBLE_LOG_DEBUG>
)
# Dev-only bench network key (64 hex chars), passed locally at configure
# time and NEVER committed; see nrf/src/nrf_provision.c and nrf/README.md.
if(DEFINED BRAMBLE_NRF_DEV_NETKEY)
  target_compile_definitions(bramble-nrf PRIVATE
    BRAMBLE_NRF_DEV_NETKEY="${BRAMBLE_NRF_DEV_NETKEY}")
endif()
# Dev-only RPC auth token, same rules as the network key above: passed
# locally at configure time, NEVER committed. Needed on consoleless boards
# (T1000-E), where the auto-minted token is logged to a UART that does not
# exist, so without this nobody can ever authenticate to the device.
if(DEFINED BRAMBLE_NRF_DEV_AUTH_TOKEN)
  target_compile_definitions(bramble-nrf PRIVATE
    BRAMBLE_NRF_DEV_AUTH_TOKEN="${BRAMBLE_NRF_DEV_AUTH_TOKEN}")
endif()
target_link_libraries(bramble-nrf PRIVATE bramble_core nrfx_lib freertos_kernel mbedcrypto monocypher_ed25519 swdr001_lib littlefs_lib nimble_lib m)
# Flash layout: `swd` (default) links at 0x0 for the dev kit; `uf2` links at
# 0x26000 for the T1000-E's stock Adafruit UF2 bootloader and also emits
# bramble-nrf.uf2 for drag-and-drop flashing.
set(BRAMBLE_NRF_LAYOUT swd CACHE STRING "Flash layout: swd or uf2")
# App base under the T1000-E's Adafruit UF2 bootloader (S140 v7.3.0 layout;
# must match ld/nrf52840_s140_app.ld FLASH ORIGIN).
set(BRAMBLE_UF2_BASE 0x27000)
# The UF2 family ID stays the generic nRF52840 one for every board; see the
# note in scripts/make_uf2.py before changing it.
if(BRAMBLE_NRF_LAYOUT STREQUAL "uf2")
  set(BRAMBLE_LD ${CMAKE_CURRENT_SOURCE_DIR}/ld/nrf52840_s140_app.ld)
  # Behind the UF2 bootloader a dead board is unreachable (no console, no
  # SWD on the bench); on boot failure the firmware stamps the boot-trace
  # page and reboots into DFU so the host can read it back.
  target_compile_definitions(bramble-nrf PRIVATE BRAMBLE_BOOT_DFU_RECOVERY=1)
else()
  set(BRAMBLE_LD ${nrfx_SOURCE_DIR}/mdk/nrf52840_xxaa.ld)
endif()
target_link_options(bramble-nrf PRIVATE
  -T${BRAMBLE_LD}
  -L${nrfx_SOURCE_DIR}/mdk
  -Wl,-Map=bramble-nrf.map
  # newlib-nano's default libc.a stubs float conversions out of printf/scanf
  # to save flash. cJSON's print_number() falls to sprintf("%1.15g", ...)
  # for any non-integral double (every GPS lat/lon in gps_events.c is one),
  # and its number parser round-trips through sscanf("%lg"); without these,
  # sprintf silently emits nothing/garbage for those fields (malformed RPC
  # JSON over BLE) and the round-trip check on parse always fails. Every
  # other cJSON number on this target happens to be integral, so nothing
  # caught this until GNSS added the first non-integral one. Flash is at
  # ~31%, so this is affordable.
  # Each as a single self-contained -Wl, token (not "-u" "SYMBOL" as two
  # list entries): CMake's link_options de-duplication otherwise collapses
  # a second bare "-u" that follows the first, silently dropping it and
  # leaving its symbol as a stray positional argument (observed: ld then
  # fails with "cannot find _scanf_float: No such file or directory").
  -Wl,--undefined=_printf_float
  -Wl,--undefined=_scanf_float
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
          --toolchain ${CMAKE_C_COMPILER_VERSION}
  COMMENT "Generating bramble-nrf.hex and enforcing the RAM budget"
)
