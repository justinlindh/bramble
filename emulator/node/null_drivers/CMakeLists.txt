# Spike scaffolding: no-op peripheral drivers for the IDF linux target.
# The sources live one level up (emulator/node/) per the task layout; this
# component exists so the IDF build can link them. Device builds never see
# this directory (only emulator/node/CMakeLists.txt adds it).
#
# All spike null drivers are gone except the wifi/OTA stubs in periph_null.c:
# radio_virt owns radio.h, display_virt owns display.h, button_virt/
# battery_virt/gps_virt own their components. esp_timer_posix.c fills the
# IDF-linux esp_timer gap.
idf_component_register(
    SRCS "../periph_null.c" "../esp_timer_posix.c" "../emu_flash_persist.c"
    REQUIRES wifi ota esp_timer freertos esp_system esp_partition
)
