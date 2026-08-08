cmake_minimum_required(VERSION 3.16)
project(bramble_tests C)

set(CMAKE_C_STANDARD 11)

# Dependencies for the nRF-backend crypto suites (test_*_nrf_backend): the
# same mbedtls + Monocypher pins (and the device's minimal mbedtls config)
# the nRF52840 target links, via the shared fragment, so the host proves
# wire-compat for the exact library the device ships. Fetched BEFORE the
# global -Werror/ASan flags below on purpose: third-party code is not held
# to this repo's warning wall, and the tests (not the libraries) are what
# ASan needs to instrument. FETCHCONTENT_BASE_DIR is shared so the ASan and
# coverage build trees download and populate these once, not twice.
set(FETCHCONTENT_BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/build-deps)
include(${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_deps.cmake)
bramble_crypto_deps_declare()
FetchContent_Declare(littlefs
    URL https://github.com/littlefs-project/littlefs/archive/refs/tags/v2.11.3.tar.gz
)
FetchContent_MakeAvailable(mbedtls monocypher littlefs)
bramble_crypto_deps_exclude_tls()

# Coverage build (BRAMBLE_COVERAGE=ON) swaps AddressSanitizer for gcov
# instrumentation: the coverage run only needs each binary to execute so gcov
# emits .gcda data, and running without ASan avoids the leak-detector's ptrace
# dependency on the CI pods. The normal build keeps ASan; the two build types
# never mix in one tree (CI uses a separate build-coverage/ directory).
option(BRAMBLE_COVERAGE "Instrument the host tests with gcov instead of ASan" OFF)
if(BRAMBLE_COVERAGE)
    add_compile_options(-Wall -Wextra -Werror --coverage -O0 -fno-omit-frame-pointer)
    add_link_options(--coverage)
else()
    add_compile_options(-Wall -Wextra -Werror -fsanitize=address -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address)
endif()

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
    ${OPENSSL_ROOT}/include
)

link_directories(${OPENSSL_ROOT}/lib)

add_executable(test_packet
    test_packet.c
    unity/unity.c
    stubs/esp_stubs.c
)

add_executable(test_wire_version
    test_wire_version.c
    unity/unity.c
    stubs/esp_stubs.c
)

add_executable(test_flag_layout
    test_flag_layout.c
    unity/unity.c
    stubs/esp_stubs.c
)

add_executable(test_aead_aad
    test_aead_aad.c
    unity/unity.c
    stubs/esp_stubs.c
)

add_executable(test_nonce_counter
    test_nonce_counter.c
    unity/unity.c
)
target_include_directories(test_nonce_counter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nonce_counter/include
)

add_executable(test_replay_window
    test_replay_window.c
    unity/unity.c
)
target_include_directories(test_replay_window PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/replay_window/include
)

add_executable(test_dm_session
    test_dm_session.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/packet.c
)
target_include_directories(test_dm_session PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dm_session/include
)
target_link_libraries(test_dm_session ssl crypto)

# Symmetric ratchet key schedule (Task 1): RK_0/CK_0 KAT vectors, per-message
# chain advance, the mandatory no-(key,nonce)-reuse gate, and the DH-ratchet
# root advance. #includes crypto_host.c + dm_session.c directly (same pattern
# as test_dm_session above).
add_executable(test_dm_ratchet
    test_dm_ratchet.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/packet.c
)
target_include_directories(test_dm_ratchet PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dm_session/include
)
target_link_libraries(test_dm_ratchet ssl crypto)

# Receiver skip / out-of-order window keyed by the cleartext ratchet index, with
# the DM_MAX_SKIP DoS bound and single-use skip cache (Task 3). Same #include-the-
# source pattern as test_dm_ratchet above.
add_executable(test_dm_ratchet_skip
    test_dm_ratchet_skip.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/packet.c
)
target_include_directories(test_dm_ratchet_skip PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dm_session/include
)
target_link_libraries(test_dm_ratchet_skip ssl crypto)

# DH ratchet on ke_epoch (Task 4): epoch-transition grace window (in-flight
# old-epoch frames still decrypt), root advance / two-sided convergence, and the
# post-grace previous-epoch wipe that delivers PCS. Same #include-the-source
# pattern as test_dm_ratchet above.
add_executable(test_dm_ratchet_epoch
    test_dm_ratchet_epoch.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/packet.c
)
target_include_directories(test_dm_ratchet_epoch PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dm_session/include
)
target_link_libraries(test_dm_ratchet_epoch ssl crypto)

# Identity-bound SAS (safety number, Task 5): the 7-digit SAS commits to the two
# pinned X25519 identity keys in addr_lo/addr_hi order, so it is stable across
# ratchet steps, epoch bumps, desync-heal, and reboot, and differs under a MitM
# key substitution. Same #include-the-source pattern as test_dm_ratchet above.
add_executable(test_dm_sas_identity
    test_dm_sas_identity.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/packet.c
)
target_include_directories(test_dm_sas_identity PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dm_session/include
)
target_link_libraries(test_dm_sas_identity ssl crypto)

# KE-in-DATA envelope: handshake transport under the channel key + the
# resulting session round-tripping a chat payload (Task 1.4).
add_executable(test_ke_envelope
    test_ke_envelope.c
    unity/unity.c
)
target_include_directories(test_ke_envelope PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/channel/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dm_session/include
)
target_link_libraries(test_ke_envelope ssl crypto)

# Location encryption: tier-in-ciphertext, canonical size across channel and
# session paths (Task 2.1 / SEC-C1). location.c is a separate source (needs
# NVS stubs + libm) rather than #include'd directly like the crypto/channel/
# dm_session components above.
add_executable(test_location_crypto
    test_location_crypto.c
    unity/unity.c
    stubs/esp_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/location.c
)
target_include_directories(test_location_crypto PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/channel/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dm_session/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/replay_window/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nonce_counter/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
)
target_compile_definitions(test_location_crypto PRIVATE NVS_STUBS_ENABLE=1)
target_link_libraries(test_location_crypto ssl crypto m)

# Network key provider: domain-separated control-plane MAC, PSK fallback
# when unprovisioned (Task 3.1, PART 3, staged not closed).
add_executable(test_network_key
    test_network_key.c
    unity/unity.c
)
target_include_directories(test_network_key PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
)
target_link_libraries(test_network_key ssl crypto)

# RREP authentication: MAC over origin-stable fields only, excluding the
# relay-mutated next_hop/header.dest_addr (Task 3.2 / SEC-H1, staged).
add_executable(test_rrep_auth
    test_rrep_auth.c
    unity/unity.c
)
target_include_directories(test_rrep_auth PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
)
target_link_libraries(test_rrep_auth ssl crypto)

# RERR authentication: MAC over broken_dest/broken_next_hop only, excluding
# the per-forwarder reporter_addr/packet_id (Task 3.3 / SEC-H1, staged).
add_executable(test_rerr_auth
    test_rerr_auth.c
    unity/unity.c
)
target_include_directories(test_rerr_auth PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing_auth/include
)
target_link_libraries(test_rerr_auth ssl crypto)

# ACK / delivery receipt authentication: MAC over src_addr/ack_packet_id
# (or orig_packet_id) only, excluding the per-hop relay_path/hop_count/
# hop_limit (Task 3.5 / NEW-SEC-8, staged).
add_executable(test_ack_auth
    test_ack_auth.c
    unity/unity.c
)
target_include_directories(test_ack_auth PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing_auth/include
)
target_link_libraries(test_ack_auth ssl crypto)

# Identity attestation relay-gate MAC (per-node identity Phase 3, Part A):
# the network-key MAC ("bramble-ident-relay-v1") over src_addr || x25519_pub
# || ed25519_pub || sig || seq that gates relay privilege, excluding the
# relay-mutable header. The test file #includes the component sources
# directly (same pattern as test_flooded_ack.c).
add_executable(test_ident_relay_auth
    test_ident_relay_auth.c
    unity/unity.c
)
target_include_directories(test_ident_relay_auth PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing_auth/include
)
target_link_libraries(test_ident_relay_auth ssl crypto)

# Identity attestation relay gate + delivery order (Phase 3, Part B):
# mirrors mesh_beacon.c's handle_identity_attestation with the real
# components in the real order (MAC first, then replay, then dedup, then
# deliver-regardless, then channel_flood_decide), including the
# bad-MAC-drop, rewritten-packet-id-replay, and relays-never-Ed-verify
# properties.
add_executable(test_ident_relay_gate
    test_ident_relay_gate.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/channel_flood.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/discovery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/routing.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/dedup.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/replay_window/replay_window.c
)
target_include_directories(test_ident_relay_gate PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing_auth/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/replay_window/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
)
target_link_libraries(test_ident_relay_gate ssl crypto)

# DATA origin authentication (Task 4-fix F1, Critical): the 8-byte network-key
# auth_hmac over the masked header + src_addr that gates reverse-route
# learning, plus the keyless-forged-DATA poisoning attack regression.
add_executable(test_data_auth
    test_data_auth.c
    unity/unity.c
)
target_include_directories(test_data_auth PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing_auth/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
)
target_link_libraries(test_data_auth ssl crypto)

# Flooding F1 Task 1: unicast DATA floods (relay + deliver) under
# s_flood_transport, reusing the SAME channel_flood_decide engine broadcast
# DATA already uses. Mirrors mesh_process_rx_packet's PKT_TYPE_DATA dispatch
# + handle_data's flood-relay gate with the real component functions (see
# test_data_auth.c's identical rationale for why mesh_task.c itself is never
# host-compiled).
add_executable(test_unicast_flood
    test_unicast_flood.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/channel_flood.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/discovery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/routing.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/dedup.c
)
target_include_directories(test_unicast_flood PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing_auth/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/include
)
target_link_libraries(test_unicast_flood ssl crypto)

# Flooding F1 Task 2: flooded-ACK sender-confirmation without routes. Mirrors
# handle_ack's flood-relay branch + the sender's pending-ack consume path with
# the real ack_sign/ack_verify, channel_flood_decide, and pending_ack
# functions (mesh_task.c itself is never host-compiled, same rationale as
# test_unicast_flood.c / test_data_auth.c).
add_executable(test_flooded_ack
    test_flooded_ack.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/channel_flood.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/discovery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/routing.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/dedup.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/reliability/reliability.c
)
target_include_directories(test_flooded_ack PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing_auth/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/reliability/include
)
target_link_libraries(test_flooded_ack ssl crypto)

# Flooding F1 Task 3: send-side flood origination + confirmation/retry
# lifecycle. Mirrors mesh_send_message's flood-vs-reactive origination gate and
# the pending_ack retry-by-re-flood loop with the real reliability, data_auth,
# and packet functions (mesh_task.c itself is never host-compiled, same
# rationale as test_unicast_flood.c / test_flooded_ack.c).
add_executable(test_flood_origination
    test_flood_origination.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/channel_flood.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/discovery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/routing.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/dedup.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/reliability/reliability.c
)
target_include_directories(test_flood_origination PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing_auth/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/reliability/include
)
target_link_libraries(test_flood_origination ssl crypto)

# Flooding F1 hardening (whole-branch review): the rebroadcast-suppression
# overheard-copy counter must act ONLY on AUTHENTICATED duplicates. Mirrors the
# two mesh_task.c dispatch-gate suppression branches (DATA via data_auth_verify,
# flooded ACK via bramble_ack_deserialize + ack_verify) with the real component
# functions, proving a bad-MAC duplicate never bumps `heard` / cancels a genuine
# pending relay while a valid one does (single flipped auth byte is the only
# difference). mesh_task.c itself is never host-compiled (same rationale as
# test_unicast_flood.c / test_flooded_ack.c).
add_executable(test_flood_suppression_auth
    test_flood_suppression_auth.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/channel_flood.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/discovery.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/routing.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/dedup.c
)
target_include_directories(test_flood_suppression_auth PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing_auth/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/include
)
target_link_libraries(test_flood_suppression_auth ssl crypto)

# Dedup tests
add_executable(test_dedup
    test_dedup.c
    unity/unity.c
)
target_include_directories(test_dedup PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/dedup/include
)

# Boot-trace page walking (nRF consoleless boards); the NVMC half stays on
# the device, this is the decision logic it drives.
add_executable(test_boot_trace
    test_boot_trace.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../nrf/src/boot_trace_scan.c
    unity/unity.c
)
target_include_directories(test_boot_trace PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nrf/src
)

# Battery percentage curve (shared by the device and virtual drivers)
add_executable(test_battery
    test_battery.c
    unity/unity.c
)
target_include_directories(test_battery PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/battery/include
)

# T1000-E battery conversion math (SAADC raw -> cell mV, divider, average)
add_executable(test_battery_t1000e
    test_battery_t1000e.c
    unity/unity.c
)
target_include_directories(test_battery_t1000e PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/battery/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../nrf/shim/include
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

# Radio bandwidth -> register-code mapping (regression guard for #149)
add_executable(test_radio_bw_mapping
    test_radio_bw_mapping.c
    unity/unity.c
)
target_include_directories(test_radio_bw_mapping PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/include
)

# CAD-timeout fail-open/closed policy (issue #118)
add_executable(test_cad_timeout_policy
    test_cad_timeout_policy.c
    unity/unity.c
)
target_include_directories(test_cad_timeout_policy PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/include
)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
)
target_link_libraries(test_identity ssl crypto)

# Trust-anchor endorsement primitive + anchor-pubkey provisioning (P0):
# canonical 58-byte message KAT, tamper rejection, and anchor set/get/
# fingerprint/persistence. Includes crypto_host.c + identity.c directly (same
# pattern as test_identity.c).
add_executable(test_identity_endorsement
    test_identity_endorsement.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/packet.c
)
target_include_directories(test_identity_endorsement PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
)
target_link_libraries(test_identity_endorsement ssl crypto)

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
# nRF target RAM-backed NVS shim tests. The shim lives in nrf/shim/ but is
# pure C, so it runs under the host suite like any component.
add_executable(test_nvs_ram_shim
    test_nvs_ram_shim.c
    ../nrf/shim/nvs_ram.c
    unity/unity.c
)
# BEFORE: the shim's nvs.h/esp_err.h must shadow test/stubs' versions.
target_include_directories(test_nvs_ram_shim BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nrf/shim/include
)
# identity_store.h supplies IDENTITY_STORE_BLOB_MAX for the capacity tests.
target_include_directories(test_nvs_ram_shim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)

# Flash-backed NVS shim: the SAME contract suite as test_nvs_ram_shim, run
# against nvs_lfs.c on littlefs's RAM block device (the NVMC driver itself is
# bench-verified). Proves the P2 storage swap preserves every documented
# semantic: type mismatch, length queries, iterators, error codes.
add_executable(test_nvs_lfs_shim
    test_nvs_ram_shim.c
    ../nrf/shim/nvs_lfs.c
    stubs/lfs_nvmc_rambd.c
    stubs/nrf_log_host_stub.c
    ${littlefs_SOURCE_DIR}/lfs.c
    ${littlefs_SOURCE_DIR}/lfs_util.c
    ${littlefs_SOURCE_DIR}/bd/lfs_rambd.c
    unity/unity.c
)
target_compile_definitions(test_nvs_lfs_shim PRIVATE
    BRAMBLE_TEST_NVS_LFS
    BRAMBLE_LFS_BASE=0
    BRAMBLE_LFS_SIZE=0x2D000
    LFS_NO_MALLOC
)
target_include_directories(test_nvs_lfs_shim BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nrf/shim/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../nrf/src
    ${littlefs_SOURCE_DIR}
)
target_include_directories(test_nvs_lfs_shim PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)

# Iterator regression suite for the same flash-backed shim. Separate from the
# shared contract suite above because it asserts nvs_lfs.c's own enforced
# single-live-iterator behaviour, which the RAM shim does not implement.
add_executable(test_nvs_lfs_iter
    test_nvs_lfs_iter.c
    ../nrf/shim/nvs_lfs.c
    stubs/lfs_nvmc_rambd.c
    stubs/nrf_log_host_stub.c
    ${littlefs_SOURCE_DIR}/lfs.c
    ${littlefs_SOURCE_DIR}/lfs_util.c
    ${littlefs_SOURCE_DIR}/bd/lfs_rambd.c
    unity/unity.c
)
target_compile_definitions(test_nvs_lfs_iter PRIVATE
    BRAMBLE_TEST_NVS_LFS
    BRAMBLE_LFS_BASE=0
    BRAMBLE_LFS_SIZE=0x2D000
    LFS_NO_MALLOC
)
target_include_directories(test_nvs_lfs_iter BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nrf/shim/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../nrf/src
    ${littlefs_SOURCE_DIR}
)
target_include_directories(test_nvs_lfs_iter PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)

# The nRF message-store backend, run for real against littlefs's RAM block
# device: recovery invariants (count from file size, torn-record truncation,
# corrupt-header recreate), the streaming rollover, and clear. The ESP SPIFFS
# variant's host test can only assert its stubs fail; this one executes the
# actual persistence code the nRF exit gate ships.
add_executable(test_msg_store_lfs
    test_msg_store_lfs.c
    ../components/msg_store/msg_store_lfs.c
    ../nrf/shim/nvs_lfs.c
    stubs/lfs_nvmc_rambd.c
    stubs/nrf_log_host_stub.c
    ${littlefs_SOURCE_DIR}/lfs.c
    ${littlefs_SOURCE_DIR}/lfs_util.c
    ${littlefs_SOURCE_DIR}/bd/lfs_rambd.c
    unity/unity.c
)
# BRAMBLE_PLATFORM_NRF goes on the backend source alone: it is the guard
# that compiles msg_store_lfs.c's body in, but nvs_lfs.c keys its RTOS-vs-
# host lock branch on the same macro and must build the host branch here.
set_source_files_properties(../components/msg_store/msg_store_lfs.c PROPERTIES
    COMPILE_DEFINITIONS BRAMBLE_PLATFORM_NRF)
target_compile_definitions(test_msg_store_lfs PRIVATE
    BRAMBLE_TEST_NVS_LFS
    BRAMBLE_LFS_BASE=0
    BRAMBLE_LFS_SIZE=0x2D000
    LFS_NO_MALLOC
)
target_include_directories(test_msg_store_lfs BEFORE PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nrf/shim/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../nrf/src
    ${littlefs_SOURCE_DIR}
)
target_include_directories(test_msg_store_lfs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)

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

# GPS duty-cycling policy (Task 9): pure module, no stubs needed.
add_executable(test_gps_duty
    test_gps_duty.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_duty.c
)
target_include_directories(test_gps_duty PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/include
)
add_test(NAME test_gps_duty COMMAND test_gps_duty)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
)
target_link_libraries(test_beacon ssl crypto)

# Discovery tests
add_executable(test_discovery
    test_discovery.c
    unity/unity.c
    stubs/esp_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/network_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
)
target_include_directories(test_discovery PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_discovery ssl crypto)

# Multi-node RREP discovery harness (real components end to end)
add_executable(test_rrep_discovery_e2e
    test_rrep_discovery_e2e.c
    unity/unity.c
    stubs/esp_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/network_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
)
target_include_directories(test_rrep_discovery_e2e PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_rrep_discovery_e2e ssl crypto)

# Forwarding tests
add_executable(test_forwarding
    test_forwarding.c
    unity/unity.c
    stubs/esp_stubs.c
)
target_include_directories(test_forwarding PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
)

# Phase 1 delivery-core Task 4: wire v4 prev_hop reverse-route learning e2e
add_executable(test_data_delivery_e2e
    test_data_delivery_e2e.c
    unity/unity.c
    stubs/esp_stubs.c
)
target_include_directories(test_data_delivery_e2e PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
)

# Phase 1 delivery-core Task 5: multi-hop channel flood relay decision
add_executable(test_channel_flood
    test_channel_flood.c
    unity/unity.c
    stubs/esp_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/network_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
)
target_include_directories(test_channel_flood PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_channel_flood ssl crypto)

# Issue #87: flood relay queue backpressure (full queue drops and counts,
# never falls back to an un-jittered immediate transmit)
add_executable(test_flood_relay_backpressure
    test_flood_relay_backpressure.c
    unity/unity.c
    stubs/esp_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/network_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
)
target_include_directories(test_flood_relay_backpressure PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_flood_relay_backpressure ssl crypto)

# Issue #75: PROBE ingress token buckets (bounded amplification, and the
# global-not-per-sender property that keeps a forged src_addr from targeting
# anyone)
add_executable(test_probe_ingress
    test_probe_ingress.c
    unity/unity.c
)
target_include_directories(test_probe_ingress PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/security/include
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

# Ed25519 signature primitive vectors (RFC 8032 KAT + malleability)
add_executable(test_ed25519
    test_ed25519.c
    unity/unity.c
)
target_include_directories(test_ed25519 PRIVATE
    stubs
    ${OPENSSL_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_ed25519 ssl crypto)

# nRF-backend crypto suites: the SAME test sources as test_crypto,
# test_crypto_vectors, and test_ed25519, rebuilt against the nRF52840
# backend (crypto_esp.c under BRAMBLE_PLATFORM_NRF: mbedtls + Monocypher
# Ed25519 via the provider seam). Passing the identical RFC 8032 / NIST /
# RFC 7748 / RFC 4231 vectors on both backends is the wire-compatibility
# proof for the port.
bramble_add_monocypher_ed25519(monocypher_ed25519_host)

foreach(crypto_suite crypto crypto_vectors ed25519)
    add_executable(test_${crypto_suite}_nrf_backend
        test_${crypto_suite}.c
        unity/unity.c
        ../components/crypto/crypto_entropy.c
        stubs/esp_random_host.c
    )
    target_compile_definitions(test_${crypto_suite}_nrf_backend PRIVATE
        BRAMBLE_TEST_NRF_BACKEND
        BRAMBLE_PLATFORM_NRF
    )
    # BEFORE: the nrf shim's esp_random.h/esp_err.h must shadow test/stubs'.
    target_include_directories(test_${crypto_suite}_nrf_backend BEFORE PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../nrf/shim/include
        ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    )
    target_link_libraries(test_${crypto_suite}_nrf_backend
        mbedcrypto
        monocypher_ed25519_host
    )
endforeach()

# Identity attestation frame: wire layout + canonical signed message +
# tamper-fails-verify (per-node identity Phase 2)
add_executable(test_identity_attestation
    test_identity_attestation.c
    unity/unity.c
    stubs/esp_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/packet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
)
target_include_directories(test_identity_attestation PRIVATE
    stubs
    ${OPENSSL_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_identity_attestation ssl crypto)

# Verified TOFU identity pin store (per-node identity Phase 3, Part C):
# first-seen-wins pinning, idempotent refresh, impersonation-conflict
# rejection, LRU-by-last-confirmed eviction, Ed25519 verify on delivery.
# The test file #includes the component sources directly (same pattern as
# test_ident_relay_auth.c / test_flooded_ack.c).
add_executable(test_identity_store
    test_identity_store.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/identity.c
)
target_include_directories(test_identity_store PRIVATE
    stubs
    ${OPENSSL_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
)
target_link_libraries(test_identity_store ssl crypto)

# Verified-pin-store PERSISTENCE (DM forward-secrecy + SAS, Task 6): the pure,
# NVS-free serialize/deserialize round-trip that carries the pin table, the
# verified bit, and the SAS-at-verification across a reboot. Same
# #include-the-source pattern as test_identity_store above.
add_executable(test_identity_store_persist
    test_identity_store_persist.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/identity.c
)
target_include_directories(test_identity_store_persist PRIVATE
    stubs
    ${OPENSSL_ROOT}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
)
target_link_libraries(test_identity_store_persist ssl crypto)

# Integration test (3-node routing)
add_executable(test_integration
    test_integration.c
    unity/unity.c
    stubs/esp_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/network_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
)
target_include_directories(test_integration PRIVATE
    stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_integration ssl crypto)

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

# Peer-location persistence. Links stubs/nvs_fake.c instead of esp_stubs.c's
# no-op NVS block (they define the same symbols, so never both): a store test
# has to read back what it wrote and walk the namespace directory.
add_executable(test_location_store
    test_location_store.c
    unity/unity.c
    stubs/nvs_fake.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/location.c
)
target_include_directories(test_location_store PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
)
target_link_libraries(test_location_store m)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_feed.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_pref.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/nmea_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/gnss_status.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/bramble_tz.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/tz_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/phy_passthrough.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/network_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_progress.c
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_feed.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_pref.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/nmea_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/gnss_status.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/bramble_tz.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/tz_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/phy_passthrough.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/network_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_progress.c
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_feed.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_pref.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/nmea_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/gnss_status.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/bramble_tz.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/tz_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/phy_passthrough.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/network_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_progress.c
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_feed.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_pref.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/nmea_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/gnss_status.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/bramble_tz.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/tz_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/phy_passthrough.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/network_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_progress.c
)

add_executable(test_broadcast_delivery_receipt
    test_broadcast_delivery_receipt.c
    unity/unity.c
    stubs/esp_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/broadcast_delivery_receipt.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/packet.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing_auth/routing_auth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/network_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/include
)
target_compile_definitions(test_rpc_add_channel_contract PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=96
)
target_compile_options(test_rpc_add_channel_contract PRIVATE
    -Wno-error=unused-parameter
)
target_link_libraries(test_rpc_add_channel_contract m ssl crypto)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/include
)
target_compile_definitions(test_rpc_set_auth_token_contract PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=96
)
target_compile_options(test_rpc_set_auth_token_contract PRIVATE
    -Wno-error=unused-parameter
)
target_link_libraries(test_rpc_set_auth_token_contract m ssl crypto)

# Trust-anchor RPC surface (P0): setAnchor / getAnchorStatus / getIdentity
# ed25519_pub, driven through the real rpc_methods.c + the real identity.c
# anchor store (same firmware-link harness as the other RPC contract tests).
add_executable(test_rpc_anchor_contract
    test_rpc_anchor_contract.c
    unity/unity.c
    stubs/esp_stubs.c
    stubs/cJSON.c
    stubs/rpc_methods_link_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_auth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/location.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_feed.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_pref.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/nmea_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/gnss_status.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/bramble_tz.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/tz_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/phy_passthrough.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/network_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_progress.c
)
target_include_directories(test_rpc_anchor_contract PRIVATE
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/include
)
target_compile_definitions(test_rpc_anchor_contract PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=96
)
target_compile_options(test_rpc_anchor_contract PRIVATE
    -Wno-error=unused-parameter
)
target_link_libraries(test_rpc_anchor_contract m ssl crypto)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/include
)
# ESP_TIMER_CUSTOM_IMPL lets the peer-location cases drive the clock: whether a
# persisted position reads as current depends on where "now" sits relative to
# the stored uptime, so a clock frozen at 0 cannot express the difference.
target_compile_definitions(test_rpc_get_config_channel_export PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=96
    ESP_TIMER_CUSTOM_IMPL=1
)
target_compile_options(test_rpc_get_config_channel_export PRIVATE
    -Wno-error=unused-parameter
)
target_link_libraries(test_rpc_get_config_channel_export m ssl crypto)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/include
)
target_compile_definitions(test_mesh_broadcast_delivery PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=96
)
target_compile_options(test_mesh_broadcast_delivery PRIVATE
    -Wno-error=unused-parameter
)
target_link_libraries(test_mesh_broadcast_delivery m ssl crypto)

target_include_directories(test_broadcast_delivery_receipt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/packet/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/routing_auth/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_compile_definitions(test_broadcast_delivery_receipt PRIVATE
    UNIT_TEST=1
)
target_link_libraries(test_broadcast_delivery_receipt ssl crypto)

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

# GPS feed core: byte assembler + sentence dispatch + position merge
add_executable(test_gps_feed
    test_gps_feed.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_feed.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/nmea_parser.c
)
target_include_directories(test_gps_feed PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
)
target_link_libraries(test_gps_feed m)

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

# Send-pipeline identity: one user message == exactly one msg_store row.
# Mirrors main/mesh_task.c's queue/flush stages (mesh_task.c is ESP-IDF-only)
# against the real msg_store uid API.
add_executable(test_msg_pipeline_uid
    test_msg_pipeline_uid.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/msg_store.c
)
target_include_directories(test_msg_pipeline_uid PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/include
)

# Delivery status surviving a reboot: the RAM store pushing a later status
# change back into the already-written record. Built with persistence ON and a
# deliberately tiny ring (4 rows against an 8-record file) so ring wrap and
# rollover both happen, which is where the ring-to-file mapping could break.
add_executable(test_msg_store_persist_status
    test_msg_store_persist_status.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/msg_store/msg_store.c
)
target_compile_definitions(test_msg_store_persist_status PRIVATE
    CONFIG_BRAMBLE_MSG_STORE_CAP=4
    CONFIG_BRAMBLE_MSG_PERSIST_ENABLED=1
    CONFIG_BRAMBLE_MSG_PERSIST_MAX=8
    CONFIG_BRAMBLE_MSG_PERSIST_ROLLOVER_KEEP_PCT=75
)
target_include_directories(test_msg_store_persist_status PRIVATE
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

# Delivery event ring tests
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/include
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
    stubs/wifi_manager_nvs_stub.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_auth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/location.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_feed.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_pref.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/nmea_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/gnss_status.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/bramble_tz.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/tz_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/phy_passthrough.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/network_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_progress.c
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/include
)
target_compile_definitions(test_rpc_methods_firmware PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=96
)
target_compile_options(test_rpc_methods_firmware PRIVATE
    -Wno-error=unused-parameter
)
target_link_libraries(test_rpc_methods_firmware m ssl crypto)

# RPC handler scratch isolation: concurrent dispatches must not share a
# snapshot buffer (issue #85). Same source set as test_rpc_methods_firmware,
# plus pthread to force the interleaving.
add_executable(test_rpc_scratch_isolation
    test_rpc_scratch_isolation.c
    unity/unity.c
    stubs/esp_stubs.c
    stubs/cJSON.c
    stubs/rpc_methods_test_stubs.c
    stubs/wifi_ws_stubs.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_auth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/location.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_feed.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_pref.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/nmea_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/gnss_status.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/bramble_tz.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/tz_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/phy_passthrough.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/network_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_progress.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/util.c
)
target_include_directories(test_rpc_scratch_isolation PRIVATE
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/include
)
target_compile_definitions(test_rpc_scratch_isolation PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=96
)
target_compile_options(test_rpc_scratch_isolation PRIVATE
    -Wno-error=unused-parameter
)
target_link_libraries(test_rpc_scratch_isolation m ssl crypto pthread)

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

# Map zoom ladder: step/clamp arithmetic and the on-screen readout (pure, no LVGL)
add_executable(test_map_zoom
    test_map_zoom.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui_graphics/map_zoom.c
)
target_include_directories(test_map_zoom PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui_graphics/include
)

# GNSS three-way state classifier and formatters (pure, no LVGL, no driver)
add_executable(test_gnss_status
    test_gnss_status.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/gnss_status.c
)
target_include_directories(test_gnss_status PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include
)

# GPS module behavior tests (ESP path with host stubs)
add_executable(test_gps
    test_gps.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_feed.c
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

# WebSocket server tests: auth enforcement on every frame, per-connection
# auth-state isolation, fail-closed paths, framing, lifecycle, and the
# notification throttle. #includes main/ws_server.c directly so its static
# handlers and static client table are reachable; the ESP-IDF httpd it sits
# on is met by the host fake in stubs/ws/. stubs/ws must precede ../main on
# the include path so its lightweight mesh_task.h shadows the real header,
# which would otherwise drag the whole mesh stack onto the host.
add_executable(test_ws_server
    test_ws_server.c
    unity/unity.c
    stubs/cJSON.c
    stubs/ws/esp_http_server_fake.c
    stubs/wifi_manager_nvs_stub.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/ws_origin.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/ws_auth_credential.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_token.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_dispatcher.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_auth.c
)
target_include_directories(test_ws_server PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs/ws
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/security/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
)
target_compile_definitions(test_ws_server PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=16
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

# Per-device SoftAP password derivation (issue #78)
add_executable(test_wifi_ap_password
    test_wifi_ap_password.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/wifi/wifi_ap_password.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
)
target_include_directories(test_wifi_ap_password PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/wifi/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_wifi_ap_password ssl crypto)

# WiFi manager tests (NVS + station/AP state transitions)
add_executable(test_wifi_manager
    test_wifi_manager.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/wifi/wifi_ap_password.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
)
target_link_libraries(test_wifi_manager ssl crypto)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
)
# Empty AP password override, matching the shipped Kconfig default, so the
# suite exercises the derived-per-device path rather than a fixed constant.
target_compile_definitions(test_wifi_manager PRIVATE
    CONFIG_BRAMBLE_WIFI_STA_TIMEOUT_S=1
    CONFIG_BRAMBLE_WIFI_SSID=""
    CONFIG_BRAMBLE_WIFI_PASSWORD=""
    CONFIG_BRAMBLE_WIFI_AP_PASSWORD=""
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_progress.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_rollback_policy.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_version.c
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_feed.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_pref.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/nmea_parser.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/gnss_status.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rpc_methods.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/bramble_tz.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/tz_store.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/phy_passthrough.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/network_key.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/identity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_url.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_progress.c
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
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/network_key/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/traffic_debug/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/include
)
target_compile_definitions(test_host_rpc_methods PRIVATE
    CONFIG_BRAMBLE_RPC_MAX_METHODS=96
)
target_compile_options(test_host_rpc_methods PRIVATE
    -Wno-error=unused-parameter
)
target_link_libraries(test_host_rpc_methods m ssl crypto)

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

# POSIX TZ local-time conversion tests (pure, no ESP deps)
add_executable(test_timezone
    test_timezone.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/bramble_tz.c
)
target_include_directories(test_timezone PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/timezone/include
)

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

# OTA progress state + throttled callback tests (pure, no ESP deps)
add_executable(test_ota_progress
    test_ota_progress.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/ota_progress.c
)
target_include_directories(test_ota_progress PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ota/include
)

# RPC auth-token minting behind the SEC-L1 entropy gate (issue #89). Compiles
# identity.c against a stubbed crypto_random so the shut-gate branch is
# reachable on the host (OpenSSL's RAND_bytes has no gate to shut).
add_executable(test_identity_token
    test_identity_token.c
    unity/unity.c
)
target_include_directories(test_identity_token PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/identity/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/nvs_keys/include
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

# BLE link encryption gate + pairing sdkconfig contract (issue #73)
add_executable(test_ble_link_sec
    test_ble_link_sec.c
    unity/unity.c
)
target_include_directories(test_ble_link_sec PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ble/include
)
target_compile_definitions(test_ble_link_sec PRIVATE
    SDKCONFIG_DEFAULTS_PATH="${CMAKE_CURRENT_SOURCE_DIR}/../sdkconfig.defaults"
)

# BLE pairing mode policy, passkey validation, pairing-failure backoff
add_executable(test_ble_pairing_policy
    test_ble_pairing_policy.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ble/ble_pairing_policy.c
)
target_include_directories(test_ble_pairing_policy PRIVATE
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

# Secure NVS init decision (SEC-H4)
add_executable(test_secure_nvs
    test_secure_nvs.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/secure_nvs.c
)
target_include_directories(test_secure_nvs PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

# RREQ pseudonym derivation (extracted from mesh_task.c; DES-12)
add_executable(test_rreq_pseudonym
    test_rreq_pseudonym.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/rreq_pseudonym.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/crypto_host.c
)
target_include_directories(test_rreq_pseudonym PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/crypto/include
)
target_link_libraries(test_rreq_pseudonym ssl crypto)

# Beacon policy pure kernels (extracted from mesh_task.c; DES-12)
add_executable(test_beacon_policy_calc
    test_beacon_policy_calc.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/beacon_policy_calc.c
)
target_include_directories(test_beacon_policy_calc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

# Probe result aggregation (extracted from mesh_task.c; DES-12)
add_executable(test_probe_results
    test_probe_results.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/probe_results.c
)
target_include_directories(test_probe_results PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

# Probe reply schedule kernel (DES-15)
add_executable(test_probe_reply
    test_probe_reply.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/probe_reply.c
)
target_include_directories(test_probe_reply PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

# emu_link framing/dispatch/send tests (Task 2; whitebox include of emu_link.c)
add_executable(test_emu_link
    test_emu_link.c
    unity/unity.c
    stubs/cJSON.c
)
target_include_directories(test_emu_link PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/emu_link/include
)
target_link_libraries(test_emu_link pthread)

# radio_esp state-reporting tests: whitebox include of radio_esp.c (normally
# ESP_PLATFORM-only) behind a fake sx1262 layer with per-command failure
# injection, covering the invariant that radio_get_state() never reports a
# capability the chip does not have.
add_executable(test_radio_esp_state
    test_radio_esp_state.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/radio_airtime.c
    unity/unity.c
)
target_include_directories(test_radio_esp_state PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/airtime/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/board_config/include
)
target_link_libraries(test_radio_esp_state m)
# radio_esp.c's host compile trips -Wtype-limits, which the host build promotes
# to an error: a uint8_t length is compared against sizeof(buf) == 256. That is
# out of scope here and not introduced by this target. The -Woverflow
# suppression this target used to carry is gone: it covered bw_hz_to_code()
# returning 500 from a uint8_t, which is deleted now that the mapping takes
# uint32_t Hz (issue #149).
target_compile_options(test_radio_esp_state PRIVATE -Wno-type-limits)

# radio_virt tests (Task 4): the virtual radio over a socketpair fake broker.
# Whitebox include of emu_link.c + radio_airtime.c + radio_profiles.c +
# radio_virt.c into one TU (same pattern as test_emu_link.c).
add_executable(test_radio_virt
    test_radio_virt.c
    unity/unity.c
    stubs/cJSON.c
)
target_include_directories(test_radio_virt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/emu_link/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/include
)
target_link_libraries(test_radio_virt pthread m)
# Virtual GPS driver: power-gate (gpsgate on/off) + NMEA-to-fix behavior
# (Task 6; whitebox include of gps_virt.c + emu_link.c + nmea_parser.c)
add_executable(test_gps_virt
    test_gps_virt.c
    unity/unity.c
    stubs/cJSON.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/gps_feed.c
)
target_include_directories(test_gps_virt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/emu_link/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/gps/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/location/include
)
target_link_libraries(test_gps_virt pthread)

# Virtual battery driver: charging-aware emu-link mapping (whitebox include
# of battery_virt.c + battery_pct.c + emu_link.c)
add_executable(test_battery_virt
    test_battery_virt.c
    unity/unity.c
    stubs/cJSON.c
)
target_include_directories(test_battery_virt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/emu_link/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/battery/include
)
target_link_libraries(test_battery_virt pthread)

# Virtual indicators driver: full-state ind emission per change
# (Task 6; whitebox include of indicator_virt.c + emu_link.c)
add_executable(test_indicators
    test_indicators.c
    unity/unity.c
    stubs/cJSON.c
)
target_include_directories(test_indicators PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/emu_link/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/indicators/include
)
target_link_libraries(test_indicators pthread)

# Pager alert pattern: beep/vibra schedule + unread LED
# (whitebox include of alerts.c against fake indicator_* backends)
add_executable(test_alerts
    test_alerts.c
    unity/unity.c
)
target_include_directories(test_alerts PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/indicators/include
)

# Virtual button driver: btn-edge dispatch + reset-exits-process
# (Task 6; whitebox include of button_virt.c + emu_link.c)
add_executable(test_button_virt
    test_button_virt.c
    unity/unity.c
    stubs/cJSON.c
)
target_include_directories(test_button_virt PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/emu_link/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/button/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include
)
target_link_libraries(test_button_virt pthread)
# SSD1680 e-paper engine tests (Task 5; datasheet-pinned command streams)
add_executable(test_ssd1680_engine
    test_ssd1680_engine.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/display/ssd1680_engine.c
)
target_include_directories(test_ssd1680_engine PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/display/include
)

# PHY passthrough gating matrix (Task 9, DESIGN.md section 10). Whitebox test
# of the pure gate state machine (phy_passthrough.c depends only on esp_timer),
# plus the real rpc_auth policy so the "enable only via an authenticated RPC"
# clause is asserted against the exact gate the dispatcher enforces, not a mock.
# ESP_TIMER_CUSTOM_IMPL lets the test drive the clock for the TTL cases.
add_executable(test_phy_passthrough
    test_phy_passthrough.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/rpc_auth.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/phy_passthrough.c
)
target_include_directories(test_phy_passthrough PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/rpc/include
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/radio/include
)
target_compile_definitions(test_phy_passthrough PRIVATE
    ESP_TIMER_CUSTOM_IMPL
)

# SAS grouping helper (pager SAS verification UX, Task 8): the pure
# "1234567" -> "123 4567" formatter. The screen that uses it (scr_sas_verify.c)
# is LVGL and not host-linkable; same include-the-source pattern as
# test_chat_message_ui.
add_executable(test_sas_format
    test_sas_format.c
    unity/unity.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/sas_format.c
)
target_include_directories(test_sas_format PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../components/ui/include
)
