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
FetchContent_MakeAvailable(nrfx cmsis6)

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
)
target_include_directories(bramble-nrf PRIVATE boards shim/include)
target_compile_definitions(bramble-nrf PRIVATE
  BRAMBLE_CONSOLE_UART=${BRAMBLE_CONSOLE_UART}
  BRAMBLE_GIT_DESCRIBE="${BRAMBLE_GIT_DESCRIBE}"
)
target_link_libraries(bramble-nrf PRIVATE nrfx_lib)
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
