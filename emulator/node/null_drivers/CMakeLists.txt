# Spike scaffolding: no-op peripheral drivers for the IDF linux target.
# The sources live one level up (emulator/node/) per the task layout; this
# component exists so the IDF build can link them. Device builds never see
# this directory (only emulator/node/CMakeLists.txt adds it).
#
# radio_null.c is gone (radio_virt owns radio.h) and periph_null.c no longer
# stubs button/battery (button_virt/battery_virt own those). display_null.c
# remains until Task 5 lands; periph_null keeps only the wifi/OTA stubs.
idf_component_register(
    SRCS "../display_null.c" "../periph_null.c" "../esp_timer_posix.c"
    REQUIRES display wifi ota esp_timer freertos esp_system
)
