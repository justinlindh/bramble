# emu_link is a host-only JSON-lines client for the emu-link broker protocol
# (emulator/DESIGN.md section 8): pthreads and BSD sockets, neither of which
# exist on-device. Its only consumers are virtual drivers (radio_virt,
# ssd1680_virt, gps_virt, ...), which only ever run on the IDF linux target.
#
# The polarity here is the mirror of components/radio and components/display:
# those gate real drivers onto the device and stand-ins onto linux. emu_link
# gates its real implementation onto linux only and registers header-only
# (no sources, so no pthread/socket dependency at all) on every other target,
# so esp32s3 builds are unaffected by this component's existence.
idf_build_get_property(target IDF_TARGET)
if(${target} STREQUAL "linux")
    idf_component_register(
        SRCS "emu_link.c"
        INCLUDE_DIRS "include"
        REQUIRES json
    )
    target_link_libraries(${COMPONENT_LIB} PRIVATE pthread)
else()
    idf_component_register(INCLUDE_DIRS "include")
    return()
endif()
