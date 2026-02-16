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
