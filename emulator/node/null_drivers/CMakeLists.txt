# Spike scaffolding: no-op peripheral drivers for the IDF linux target.
# The sources live one level up (emulator/node/) per the task layout; this
# component exists so the IDF build can link them. Device builds never see
# this directory (only emulator/node/CMakeLists.txt adds it).
idf_component_register(
    SRCS "../radio_null.c" "../display_null.c" "../periph_null.c" "../esp_timer_posix.c"
    REQUIRES radio display button battery wifi ota ui esp_timer freertos esp_system
)
