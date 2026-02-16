cmake_minimum_required(VERSION 3.16)
project(bramble_tests C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Werror -fsanitize=address -fno-omit-frame-pointer)
add_link_options(-fsanitize=address)

set(OPENSSL_ROOT "/home/linuxbrew/.linuxbrew/opt/openssl@3")

include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}/unity
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet
    ${OPENSSL_ROOT}/include
)

link_directories(${OPENSSL_ROOT}/lib)

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

# Crypto tests
add_executable(test_crypto
    test_crypto.c
    unity/unity.c
)
target_include_directories(test_crypto PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_crypto ssl crypto)

# Identity tests
add_executable(test_identity
    test_identity.c
    unity/unity.c
)
target_include_directories(test_identity PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/include
)
target_link_libraries(test_identity ssl crypto)

# Channel key tests
add_executable(test_channel_key
    test_channel_key.c
    unity/unity.c
)
target_include_directories(test_channel_key PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/channel/include
)
target_link_libraries(test_channel_key ssl crypto)

# Key exchange tests
add_executable(test_key_exchange
    test_key_exchange.c
    unity/unity.c
)
target_include_directories(test_key_exchange PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_key_exchange ssl crypto)

# RREQ privacy tests
add_executable(test_rreq_privacy
    test_rreq_privacy.c
    unity/unity.c
)
target_include_directories(test_rreq_privacy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_rreq_privacy ssl crypto)

# Neighbor table tests
add_executable(test_neighbor
    test_neighbor.c
    unity/unity.c
    stubs/esp_stubs.c
)
target_include_directories(test_neighbor PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
)

# Routing table tests
add_executable(test_routing
    test_routing.c
    unity/unity.c
    stubs/esp_stubs.c
)
target_include_directories(test_routing PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
)

# Reliability tests
add_executable(test_reliability
    test_reliability.c
    unity/unity.c
    stubs/esp_stubs.c
)
target_include_directories(test_reliability PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/reliability/include
)

# Airtime budget tests
add_executable(test_airtime_budget
    test_airtime_budget.c
    unity/unity.c
)
target_include_directories(test_airtime_budget PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/include
)

# TX queue tests
add_executable(test_tx_queue
    test_tx_queue.c
    unity/unity.c
)
target_include_directories(test_tx_queue PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/include
)

# Timesync tests
add_executable(test_timesync
    test_timesync.c
    unity/unity.c
)
target_include_directories(test_timesync PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timesync/include
)

# Anti-replay tests
add_executable(test_anti_replay
    test_anti_replay.c
    unity/unity.c
)
target_include_directories(test_anti_replay PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timesync/include
)

# Beacon tests
add_executable(test_beacon
    test_beacon.c
    unity/unity.c
    stubs/esp_stubs.c
)
target_include_directories(test_beacon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_beacon ssl crypto)

# Discovery tests
add_executable(test_discovery
    test_discovery.c
    unity/unity.c
    stubs/esp_stubs.c
)
target_include_directories(test_discovery PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
)

# Forwarding tests
add_executable(test_forwarding
    test_forwarding.c
    unity/unity.c
    stubs/esp_stubs.c
)
target_include_directories(test_forwarding PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
)

# Channel flood tests
add_executable(test_channel_flood
    test_channel_flood.c
    unity/unity.c
)
target_include_directories(test_channel_flood PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/include
)

# Fragment tests
add_executable(test_fragment
    test_fragment.c
    unity/unity.c
)
target_include_directories(test_fragment PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/fragment/include
)

# Channel message tests
add_executable(test_channel_msg
    test_channel_msg.c
    unity/unity.c
)
target_include_directories(test_channel_msg PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/channel/include
)
target_link_libraries(test_channel_msg ssl crypto)

# Security tests
add_executable(test_security
    test_security.c
    unity/unity.c
)
target_include_directories(test_security PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/security/include
)
