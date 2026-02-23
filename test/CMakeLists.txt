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

# Channel storage tests (Phase 1 persistence)
add_executable(test_channel_storage
    test_channel_storage.c
    unity/unity.c
    stubs/esp_stubs.c
)
target_include_directories(test_channel_storage PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/channel/include
)
target_link_libraries(test_channel_storage ssl crypto)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timesync/timesync.c
)
target_include_directories(test_timesync PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timesync/include
)

# Frequency plan tests
add_executable(test_freq_plan
    test_freq_plan.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/freq_plan/freq_plan.c
)
target_include_directories(test_freq_plan PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/freq_plan/include
)

# Airtime budget math tests
add_executable(test_airtime
    test_airtime.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/airtime_budget.c
)
target_include_directories(test_airtime PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/include
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

# UI state machine + formatter tests
add_executable(test_ui
    test_ui.c
    unity/unity.c
)
target_include_directories(test_ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include
)

# JSON-RPC parser tests
add_executable(test_json_rpc
    test_json_rpc.c
    unity/unity.c
)
target_include_directories(test_json_rpc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ble/include
)

# Crypto test vectors (NIST/RFC)
add_executable(test_crypto_vectors
    test_crypto_vectors.c
    unity/unity.c
)
target_include_directories(test_crypto_vectors PRIVATE
    stubs
    ${OPENSSL_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_crypto_vectors ssl crypto)

# Integration test (3-node routing)
add_executable(test_integration
    test_integration.c
    unity/unity.c
    stubs/esp_stubs.c
)
target_include_directories(test_integration PRIVATE
    stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/include
)

# Key backup tests
add_executable(test_key_backup
    test_key_backup.c
    unity/unity.c
)
target_include_directories(test_key_backup PRIVATE
    stubs
    ${OPENSSL_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ble/include
)
target_link_libraries(test_key_backup ssl crypto)

# Dummy traffic tests
add_executable(test_dummy_traffic
    test_dummy_traffic.c
    unity/unity.c
    stubs/esp_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
)
target_include_directories(test_dummy_traffic PRIVATE
    stubs
    ${OPENSSL_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/security/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_dummy_traffic ssl crypto)

# Beacon routes tests
add_executable(test_beacon_routes
    test_beacon_routes.c
    unity/unity.c
    stubs/esp_stubs.c
)
target_include_directories(test_beacon_routes PRIVATE
    stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
)

# Mailbox tests (includes .c directly)
add_executable(test_mailbox
    test_mailbox.c
    unity/unity.c
)
target_include_directories(test_mailbox PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mailbox/include
)

# Emergency beacon tests (custom test harness, links .c)
add_executable(test_emergency
    test_emergency.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/emergency/emergency.c
)
target_include_directories(test_emergency PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/emergency/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
)

# Location sharing tests
add_executable(test_location
    test_location.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/location.c
)
target_include_directories(test_location PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
)
target_link_libraries(test_location m)

# Group DM tests (custom test harness)
add_executable(test_group
    test_group.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/group/group.c
)
target_include_directories(test_group PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/group/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
)

# Network coding tests (custom test harness)
add_executable(test_coding
    test_coding.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/coding/coding.c
)
target_include_directories(test_coding PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/coding/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
)

# Adaptive routing metric tests
add_executable(test_route_metric
    test_route_metric.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/route_metric.c
)
target_include_directories(test_route_metric PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
)

# Public channel tests (includes .c directly, needs OpenSSL)
add_executable(test_public_channel
    test_public_channel.c
    unity/unity.c
)
target_include_directories(test_public_channel PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/channel/include
)
target_link_libraries(test_public_channel ssl crypto)

# RPC dispatcher tests
add_executable(test_rpc_dispatcher
    test_rpc_dispatcher.c
    unity/unity.c
    stubs/esp_stubs.c
    stubs/cJSON.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_dispatcher.c
)
target_include_directories(test_rpc_dispatcher PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/include
)
target_compile_definitions(test_rpc_dispatcher PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=16
)

# RPC method parameter/error behavior tests
add_executable(test_rpc_methods
    test_rpc_methods.c
    unity/unity.c
    stubs/esp_stubs.c
    stubs/cJSON.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_dispatcher.c
)
target_include_directories(test_rpc_methods PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/include
)
target_compile_definitions(test_rpc_methods PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=16
)

# RPC addChannel contract tests against firmware rpc handler
add_executable(test_rpc_add_channel_contract
    test_rpc_add_channel_contract.c
    unity/unity.c
    stubs/esp_stubs.c
    stubs/cJSON.c
    stubs/rpc_methods_link_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
)
add_executable(test_rpc_get_config_channel_export
    test_rpc_get_config_channel_export.c
    unity/unity.c
    stubs/esp_stubs.c
    stubs/cJSON.c
    stubs/rpc_methods_link_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
)
target_include_directories(test_rpc_add_channel_contract PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/security/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/channel/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/freq_plan/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/display/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/battery/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
)
target_compile_definitions(test_rpc_add_channel_contract PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=40
)
target_compile_options(test_rpc_add_channel_contract PRIVATE
    -Wno-error=unused-parameter
)

target_include_directories(test_rpc_get_config_channel_export PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/security/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/channel/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/freq_plan/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/display/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/battery/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
)
target_compile_definitions(test_rpc_get_config_channel_export PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=40
)
target_compile_options(test_rpc_get_config_channel_export PRIVATE
    -Wno-error=unused-parameter
)

# NMEA parser tests (pure C, no ESP-IDF deps)
add_executable(test_nmea_parser
    test_nmea_parser.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/nmea_parser.c
)
target_include_directories(test_nmea_parser PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
)
target_link_libraries(test_nmea_parser m)

# Chat target helper tests
add_executable(test_chat_target
    test_chat_target.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui_graphics/chat_target.c
)
target_include_directories(test_chat_target PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui_graphics/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/include
)

# Chat unread helper tests
add_executable(test_chat_unread
    test_chat_unread.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui_graphics/chat_unread.c
)
target_include_directories(test_chat_unread PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui_graphics/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/include
)

# Message store channel metadata tests
add_executable(test_msg_store
    test_msg_store.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/msg_store.c
)
target_include_directories(test_msg_store PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/include
)

# Message store SPIFFS persistence tests
add_executable(test_msg_store_spiffs
    test_msg_store_spiffs.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/msg_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/msg_store_spiffs.c
)
target_include_directories(test_msg_store_spiffs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/include
)

# Probe v2 invariant/spec tests
add_executable(test_probe_v2_spec
    test_probe_v2_spec.c
    unity/unity.c
)
target_include_directories(test_probe_v2_spec PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/bramble_probe/include
)

# Traffic debug telemetry tests
add_executable(test_traffic_debug
    test_traffic_debug.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/traffic_debug.c
)
target_include_directories(test_traffic_debug PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
)
