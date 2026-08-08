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
         "../emu_provision.c" "../emu_autosend.c"
    REQUIRES wifi ota esp_timer freertos esp_system esp_partition network_key
             location nvs_keys nvs_flash
)

# The emulator node runs with the firmware's REAL task priorities, the same as
# device builds (issue #50): the previous global priority-flattening workaround
# is removed. Flattening masked a scheduler-corruption abort that surfaced under
# real priorities; the actual cause was the emu_link reader pthread being
# created without the parent blocking signals first, so a process-directed
# SIGALRM tick could land on that raw thread during its startup window and run
# the scheduler on it (fixed in components/emu_link/emu_link.c). With that race
# closed, real priorities run correctly, including under single-core CPU
# starvation, and let the emulator match device scheduling instead of hiding
# priority-design bugs.
