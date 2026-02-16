cmake_minimum_required(VERSION 3.16)
project(bramble_tests C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Werror -fsanitize=address -fno-omit-frame-pointer)
add_link_options(-fsanitize=address)

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/unity
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet
)

add_executable(test_packet
    test_packet.c
    unity/unity.c
    stubs/esp_stubs.c
)

# Dedup tests
add_executable(test_dedup
    test_dedup.c
    unity/unity.c
)
target_include_directories(test_dedup PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/include
)

# Radio airtime tests
add_executable(test_radio_airtime
    test_radio_airtime.c
    unity/unity.c
)
target_include_directories(test_radio_airtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/include
)
target_link_libraries(test_radio_airtime m)
