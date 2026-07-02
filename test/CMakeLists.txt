cmake_minimum_required(VERSION 3.16)
project(bramble_tests C)

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Werror -fsanitize=address -fno-omit-frame-pointer)
add_link_options(-fsanitize=address)

# Locate OpenSSL portably (was hardcoded to a linuxbrew path; DES-19).
# OPENSSL_ROOT is derived from the discovered include dir so the existing
# ${OPENSSL_ROOT}/include and ${OPENSSL_ROOT}/lib references keep working.
find_package(OpenSSL REQUIRED)
get_filename_component(OPENSSL_ROOT "${OPENSSL_INCLUDE_DIR}" DIRECTORY)

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

# RERR ACK fast-fail tests
add_executable(test_rerr_ack_fastfail
    test_rerr_ack_fastfail.c
    unity/unity.c
    stubs/esp_stubs.c
)
target_include_directories(test_rerr_ack_fastfail PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/reliability/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

# Airtime budget tests
add_executable(test_airtime_budget
    test_airtime_budget.c
    unity/unity.c
)
target_include_directories(test_airtime_budget PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/include
)

# TX gate ToA cost tests (DES-7)
add_executable(test_tx_gate_cost
    test_tx_gate_cost.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/tx_gate.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/radio_airtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/airtime_budget.c
)
target_include_directories(test_tx_gate_cost PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/include
)
target_link_libraries(test_tx_gate_cost m)
add_test(NAME test_tx_gate_cost COMMAND test_tx_gate_cost)

# TX gate chokepoint tests (DES-5, DES-6)
add_executable(test_tx_gate
    test_tx_gate.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/tx_gate.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/radio_airtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/airtime_budget.c
)
target_include_directories(test_tx_gate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/include
)
target_link_libraries(test_tx_gate m)
add_test(NAME test_tx_gate COMMAND test_tx_gate)

# Beacon budget-fit tests (review BLOCKER on PR #82)
add_executable(test_tx_beacon_fit
    test_tx_beacon_fit.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/tx_gate.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/radio_airtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/airtime_budget.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/freq_plan/freq_plan.c
)
target_include_directories(test_tx_beacon_fit PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/freq_plan/include
)
target_link_libraries(test_tx_beacon_fit m)
add_test(NAME test_tx_beacon_fit COMMAND test_tx_beacon_fit)

# Retry-storm budget tests (DES-6)
add_executable(test_tx_retry_budget
    test_tx_retry_budget.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/tx_gate.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/radio_airtime.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/airtime_budget.c
)
target_include_directories(test_tx_retry_budget PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/include
)
target_link_libraries(test_tx_retry_budget m)
add_test(NAME test_tx_retry_budget COMMAND test_tx_retry_budget)

# Duty-cycle cap tests (DES-8)
add_executable(test_duty_cycle
    test_duty_cycle.c
    unity/unity.c
)
target_include_directories(test_duty_cycle PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/include
)
add_test(NAME test_duty_cycle COMMAND test_duty_cycle)

# Broadcast airtime gate tests
add_executable(test_broadcast_airtime_gate
    test_broadcast_airtime_gate.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/airtime_budget.c
)
target_include_directories(test_broadcast_airtime_gate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/include
)
add_test(NAME test_broadcast_airtime_gate COMMAND test_broadcast_airtime_gate)

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

# DATA relay AAD tests (hop_limit mutation vs GCM header binding)
add_executable(test_data_relay_aad
    test_data_relay_aad.c
    unity/unity.c
)
target_include_directories(test_data_relay_aad PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/channel/include
)
target_link_libraries(test_data_relay_aad ssl crypto)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
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

# Mailbox tests (includes .c directly)
add_executable(test_mailbox
    test_mailbox.c
    unity/unity.c
)
target_include_directories(test_mailbox PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mailbox/include
)

# Location sharing tests
add_executable(test_location
    test_location.c
    unity/unity.c
    stubs/esp_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/location.c
)
target_include_directories(test_location PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
)
target_compile_definitions(test_location PRIVATE NVS_STUBS_ENABLE=1)
target_link_libraries(test_location m)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_auth.c
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_auth.c
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_auth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/location.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
)
add_executable(test_rpc_set_auth_token_contract
    test_rpc_set_auth_token_contract.c
    unity/unity.c
    stubs/esp_stubs.c
    stubs/cJSON.c
    stubs/rpc_methods_link_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_auth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/location.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
)
add_executable(test_rpc_get_config_channel_export
    test_rpc_get_config_channel_export.c
    unity/unity.c
    stubs/esp_stubs.c
    stubs/cJSON.c
    stubs/rpc_methods_link_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_auth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/location.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
)
add_executable(test_mesh_broadcast_delivery
    test_mesh_broadcast_delivery.c
    unity/unity.c
    stubs/esp_stubs.c
    stubs/cJSON.c
    stubs/rpc_methods_link_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_auth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/location.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
)

add_executable(test_broadcast_delivery_receipt
    test_broadcast_delivery_receipt.c
    unity/unity.c
    stubs/esp_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/broadcast_delivery_receipt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/packet.c
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
)
target_compile_definitions(test_rpc_add_channel_contract PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=40
)
target_compile_options(test_rpc_add_channel_contract PRIVATE
    -Wno-error=unused-parameter
)
target_link_libraries(test_rpc_add_channel_contract m)

target_include_directories(test_rpc_set_auth_token_contract PRIVATE
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
)
target_compile_definitions(test_rpc_set_auth_token_contract PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=40
)
target_compile_options(test_rpc_set_auth_token_contract PRIVATE
    -Wno-error=unused-parameter
)
target_link_libraries(test_rpc_set_auth_token_contract m)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
)
target_compile_definitions(test_rpc_get_config_channel_export PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=40
)
target_compile_options(test_rpc_get_config_channel_export PRIVATE
    -Wno-error=unused-parameter
)
target_link_libraries(test_rpc_get_config_channel_export m)

target_include_directories(test_mesh_broadcast_delivery PRIVATE
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
)
target_compile_definitions(test_mesh_broadcast_delivery PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=40
)
target_compile_options(test_mesh_broadcast_delivery PRIVATE
    -Wno-error=unused-parameter
)
target_link_libraries(test_mesh_broadcast_delivery m)

target_include_directories(test_broadcast_delivery_receipt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
)
target_compile_definitions(test_broadcast_delivery_receipt PRIVATE
    UNIT_TEST=1
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

# Delivery event ring persistence tests
add_executable(test_delivery_event_ring
    test_delivery_event_ring.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/delivery_event_ring.c
)
target_include_directories(test_delivery_event_ring PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)
target_compile_definitions(test_delivery_event_ring PRIVATE
    DELIVERY_EVENT_RING_CAPACITY=4
)

# Probe v2 invariant/spec tests
add_executable(test_probe_v2_spec
    test_probe_v2_spec.c
    unity/unity.c
)
target_include_directories(test_probe_v2_spec PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/bramble_probe/include
)

# Probe rate-limit token bucket tests
add_executable(test_probe_ratelimit
    test_probe_ratelimit.c
    unity/unity.c
)
target_include_directories(test_probe_ratelimit PRIVATE
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

# Board profile contract tests (Heltec V4)
add_executable(test_board_profiles
    test_board_profiles.c
    unity/unity.c
    stubs/esp_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/board.c
)
target_include_directories(test_board_profiles PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board_config/include
)
target_compile_definitions(test_board_profiles PRIVATE
    CONFIG_BRAMBLE_BOARD_HELTEC_V4=1
)

# RPC methods firmware handler tests (real rpc_methods.c, correct stub signatures)
add_executable(test_rpc_methods_firmware
    test_rpc_methods_firmware.c
    unity/unity.c
    stubs/esp_stubs.c
    stubs/cJSON.c
    stubs/rpc_methods_test_stubs.c
    stubs/wifi_ws_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_auth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/location.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/util.c
)
target_include_directories(test_rpc_methods_firmware PRIVATE
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
)
target_compile_definitions(test_rpc_methods_firmware PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=40
)
target_compile_options(test_rpc_methods_firmware PRIVATE
    -Wno-error=unused-parameter
)
target_link_libraries(test_rpc_methods_firmware m)

# Chat message UI helper tests
add_executable(test_chat_message_ui
    test_chat_message_ui.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui_graphics/chat_message_ui.c
)
target_include_directories(test_chat_message_ui PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui_graphics/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/include
)

# GPS module behavior tests (ESP path with host stubs)
add_executable(test_gps
    test_gps.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/nmea_parser.c
)
target_include_directories(test_gps PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board_config/include
)
target_compile_definitions(test_gps PRIVATE
    ESP_PLATFORM=1
    FREERTOS_TASK_CUSTOM_IMPL=1
    ESP_TIMER_CUSTOM_IMPL=1
)
target_link_libraries(test_gps m)

# WS auth tests: constant-time compare + unauthenticated allowlist policy
add_executable(test_ws_auth
    test_ws_auth.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_auth.c
)
target_include_directories(test_ws_auth PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/include
)

# WS Origin allowlist policy tests (pure decision unit)
add_executable(test_ws_origin
    test_ws_origin.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/ws_origin.c
)
target_include_directories(test_ws_origin PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

# Audio module behavior tests (NVS + I2S lifecycle with stubs)
add_executable(test_audio
    test_audio.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/audio/audio.c
)
target_include_directories(test_audio PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/audio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board_config/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
)
target_compile_definitions(test_audio PRIVATE
    FREERTOS_TASK_CUSTOM_IMPL=1
)
target_link_libraries(test_audio m)

target_compile_options(test_audio PRIVATE
    -Wno-error=unused-parameter
)

# WiFi manager tests (NVS + station/AP state transitions)
add_executable(test_wifi_manager
    test_wifi_manager.c
    unity/unity.c
)

# Button driver tests (debounce, short/long/double press timing)
add_executable(test_button
    host/test_button.c
    unity/unity.c
)
target_include_directories(test_button PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/button/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board_config/include
)
target_include_directories(test_wifi_manager PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/wifi/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
)
target_compile_definitions(test_wifi_manager PRIVATE
    CONFIG_BRAMBLE_WIFI_STA_TIMEOUT_S=1
    CONFIG_BRAMBLE_WIFI_SSID=""
    CONFIG_BRAMBLE_WIFI_PASSWORD=""
    CONFIG_BRAMBLE_WIFI_AP_PASSWORD="testpass"
)
target_compile_options(test_wifi_manager PRIVATE
    -Wno-error=unused-parameter
    -fno-sanitize=address
)
target_link_options(test_wifi_manager PRIVATE
    -fno-sanitize=address
)

# OTA URL routing + security guardrail tests
add_executable(test_ota
    test_ota.c
    unity/unity.c
)
target_include_directories(test_ota PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
)
target_compile_options(test_ota PRIVATE
    -Wno-error=unused-parameter
)

# Host-side RPC method handler tests (T3): pure-logic methods without hardware
# Covers: ping, getVersion, getConfig, setAuthToken
add_executable(test_host_rpc_methods
    host/test_rpc_methods.c
    unity/unity.c
    stubs/esp_stubs.c
    stubs/cJSON.c
    stubs/rpc_methods_test_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_auth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/location.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/util.c
)
target_include_directories(test_host_rpc_methods PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
)
target_compile_definitions(test_host_rpc_methods PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=40
)
target_compile_options(test_host_rpc_methods PRIVATE
    -Wno-error=unused-parameter
)
target_link_libraries(test_host_rpc_methods m)

# Mailbox integration wrapper tests (mesh_task.c wrapper logic, no ESP-IDF deps)
# Tests: store-when-disabled, store-when-enabled, flush-for, expire/TTL
add_executable(test_mailbox_wrappers
    test_mailbox_wrappers.c
    unity/unity.c
)
target_include_directories(test_mailbox_wrappers PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/mailbox/include
)
add_test(NAME test_mailbox_wrappers COMMAND test_mailbox_wrappers)

# OTA URL policy tests (pure, no ESP deps)
add_executable(test_ota_url
    test_ota_url.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
)
target_include_directories(test_ota_url PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
)

# OTA semver / anti-rollback comparison tests (pure, no ESP deps)
add_executable(test_ota_version
    test_ota_version.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_version.c
)
target_include_directories(test_ota_version PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
)

# Entropy gate tests (SEC-L1)
add_executable(test_crypto_entropy
    test_crypto_entropy.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_entropy.c
)
target_include_directories(test_crypto_entropy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)

# BLE log redaction predicate (NEW-SEC-5)
add_executable(test_ble_redact
    test_ble_redact.c
    unity/unity.c
)
target_include_directories(test_ble_redact PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ble/include
)

# WS credential parser: header-only auth, no query string (NEW-SEC-6)
add_executable(test_ws_auth_credential
    test_ws_auth_credential.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/ws_auth_credential.c
)
target_include_directories(test_ws_auth_credential PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

# Secure partition table contract (SEC-H4)
add_executable(test_secure_partitions
    test_secure_partitions.c
    unity/unity.c
)
target_compile_definitions(test_secure_partitions PRIVATE
    SECURE_PARTITIONS_CSV="${CMAKE_CURRENT_SOURCE_DIR}/../partitions.secure.csv"
)
