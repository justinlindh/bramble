# Spike scaffolding: no-op peripheral drivers for the IDF linux target.
# The sources live one level up (emulator/node/) per the task layout; this
# component exists so the IDF build can link them. Device builds never see
# this directory (only emulator/node/CMakeLists.txt adds it).
#
# radio_null.c is gone: the radio.h implementation now comes from
# components/radio/radio_virt.c (the emu_link ether client). display/periph
# stubs remain until Tasks 5/6 replace them.
idf_component_register(
    SRCS "../display_null.c" "../periph_null.c" "../esp_timer_posix.c"
    REQUIRES display button battery wifi ota ui esp_timer freertos esp_system
)
