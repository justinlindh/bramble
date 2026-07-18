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
         "../emu_provision.c" "../emu_autosend.c" "../emu_task_flatten.c"
    REQUIRES wifi ota esp_timer freertos esp_system esp_partition network_key
)

# Priority flattening (emu_task_flatten.c): route every task-creation call in
# the final executable, including IDF-internal ones, through the clamping
# wrappers. See that file's header for why unequal task priorities freeze the
# linux-port node under load. INTERFACE so the option lands on the app link.
target_link_options(${COMPONENT_LIB} INTERFACE
    "-Wl,--wrap=xTaskCreate"
    "-Wl,--wrap=xTaskCreatePinnedToCore"
    "-Wl,--wrap=xTaskCreateStatic"
    "-Wl,--wrap=xTaskCreateStaticPinnedToCore"
    "-Wl,--wrap=vTaskPrioritySet")
