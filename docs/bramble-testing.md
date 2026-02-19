# Bramble Testing Reference

Quick reference for all Bramble test systems. Read this before making changes to understand what to run and when.

---

## 1. Core Unit Tests (C / Unity)

**What:** 34 test suites covering every firmware component — crypto, routing, packet encoding, airtime budgets, UI state machine, RPC dispatcher, channels, mailbox, location, groups, network coding, etc.

**Where:** `bramble/test/`

**Run:**
```bash
cd /home/user/.local/workspace/bramble
bash test/run_all_tests.sh
```

**How it works:** Builds with CMake + GCC on the host (not ESP-IDF). Uses Unity test framework with ESP-IDF stubs (`test/stubs/`). Compiled with `-fsanitize=address` for memory safety. Requires OpenSSL 3 (via linuxbrew at `/home/linuxbrew/.linuxbrew/opt/openssl@3`).

**When to run:**
- After ANY change to `components/` source files
- Before committing firmware changes
- Before flashing hardware

**Test suites:**
| Suite | Component | Tests |
|-------|-----------|-------|
| test_packet | Packet encode/decode | Header fields, wire format |
| test_crypto | X25519 + AES-256-GCM | Key gen, encrypt/decrypt |
| test_crypto_vectors | NIST/RFC vectors | Standards compliance |
| test_identity | Node identity | Address derivation |
| test_key_exchange | ECDH handshake | Full key exchange flow |
| test_key_backup | Key backup/restore | Physical button auth |
| test_rreq_privacy | Encrypted RREQ source | Salt rotation |
| test_routing | Route table | Add/remove/lookup/expire |
| test_neighbor | Neighbor table | Add/update/evict |
| test_discovery | Route discovery | RREQ/RREP generation |
| test_forwarding | Multi-hop forwarding | Hop limit, relay |
| test_beacon | Beacon encode/decode | Name field, backward compat |
| test_beacon_routes | Route ads in beacons | Piggyback routes |
| test_dedup | Dedup cache | Duplicate detection, expiry |
| test_reliability | 3-tier reliability | ACK, retry, timeout |
| test_airtime_budget | Airtime budgets | Tier rates, exhaustion |
| test_tx_queue | TX queue | Priority, scheduling |
| test_radio_airtime | LoRa airtime calc | ToA formula |
| test_timesync | Time synchronization | Drift compensation |
| test_anti_replay | Replay protection | Window-based detection |
| test_fragment | Fragmentation | Split/reassemble |
| test_channel_key | Channel PSK | Key derivation |
| test_channel_msg | Channel messages | Encrypt/decrypt |
| test_channel_flood | Channel flood control | Rate limiting |
| test_public_channel | Public channels | Unencrypted broadcast |
| test_mailbox | Store-and-forward | Buffer, expiry, flush |
| test_emergency | Emergency beacons | Priority, format |
| test_location | Location sharing | Encode/decode coords |
| test_group | Group DMs | Membership, key rotation |
| test_coding | Network coding | XOR coding/decoding |
| test_route_metric | Adaptive routing | ETX, composite metric |
| test_security | Security policies | Rate limits, anomaly |
| test_ui | OLED UI state machine | Transitions, formatting |
| test_json_rpc | JSON-RPC parser | Method dispatch, errors |
| test_rpc_dispatcher | RPC dispatcher | Handler registration |
| test_integration | 3-node routing | Multi-hop end-to-end |

---

## 2. Go SDK Tests

**What:** Client and protocol-level tests using a mock transport. Verifies JSON-RPC serialization, version negotiation, notification handling.

**Where:** `bramble-go/client_test.go`, `bramble-go/protocol_test.go`

**Run:**
```bash
cd /home/user/.local/workspace/bramble-go
go test ./...
```

**When to run:**
- After changes to bramble-go SDK code
- After modifying RPC method signatures in firmware
- Before tagging SDK releases

---

## 3. Webapp Tests (Vitest)

**What:** 47 tests covering Zustand store actions (message state transitions), SerialTransport protocol, and channel/node share URL encoding.

**Where:** `bramble/webapp/test/`

**Run:**
```bash
cd /home/user/.local/workspace/bramble/webapp
npx vitest run
```

**Test files:**
| File | Coverage |
|------|----------|
| `test/store/actions.test.ts` | Message lifecycle, delivery status, neighbor updates (9 tests) |
| `test/store/airtime.test.ts` | Airtime normalizer: firmware→tier mapping, percentages, refill countdown, broadcast regression, edge cases (13 tests) |
| `test/transport/SerialTransport.test.ts` | Web Serial JSON-RPC, connect/disconnect, RPC timeout (7 tests) |
| `test/utils/channelShare.test.ts` | `bramble://` URL encode/decode for channels and nodes (18 tests) |

**Known issue:** `SerialTransport > rejects pending RPCs on disconnect` times out intermittently (race condition in mock reader teardown). Non-blocking.

**When to run:**
- After changes to `webapp/src/` files
- Before committing webapp changes

---

## 4. Simulator Scenarios

**What:** Go simulation engine with 24 scenarios testing mesh behavior at scale (2–100+ nodes). Validates routing convergence, message delivery, airtime budgets, anomaly detection, emergency beacons, etc.

**Where:** `bramble/simulator/`

**Run one scenario:**
```bash
cd /home/user/.local/workspace/bramble/simulator
bash scripts/run-scenario.sh ideal-10-node
```

**Run all scenarios:**
```bash
bash scripts/run-scenario.sh all
```

**Run E2E smoke test (2-node):**
```bash
cd /home/user/.local/workspace/bramble/simulator
bash test-e2e.sh
```

**Scenarios:**
| Scenario | What it tests |
|----------|---------------|
| test-2-node | Minimal smoke test |
| 3-node-linear | Linear relay chain |
| 10-node-grid | Grid topology routing |
| ideal-10-node | Ideal conditions delivery |
| ideal-massive | 50+ node stress test |
| stress-test | High message rate |
| reliability-ack-retry | ACK/retry mechanism |
| reliability-path-trace | Multi-hop path tracking |
| adaptive-routing | ETX metric convergence |
| dedup-flood | Flood dedup performance |
| fragment-large-message | Fragmentation |
| crypto-overhead | Encryption performance |
| mailbox-store-forward | Offline message delivery |
| emergency-beacon | Emergency priority |
| location-sharing | GPS coordinate sharing |
| group-dm | Group messaging |
| public-channel-broadcast | Broadcast delivery |
| network-coding-relay | XOR coding efficiency |
| airtime-exhaustion | Budget enforcement |
| field-deployment | Real-world conditions |
| anomaly-black-hole | Black hole detection |
| anomaly-excessive-rreq | RREQ flood detection |
| anomaly-partition | Network partition handling |
| anomaly-route-loop | Loop detection |

**When to run:**
- After routing or mesh protocol changes
- After airtime/reliability/security changes
- Before major releases (run all)

---

## 5. Hardware E2E Tests (Python)

**What:** 23 RPC method tests against real Bramble hardware. Exercises ping, identity, status, config, channels, messaging, location, probing over actual transport (WebSocket, BLE, or Serial).

**Where:** `bramble/scripts/e2e-test.py`

**Run (requires 2 powered-on boards):**
```bash
cd /home/user/.local/workspace/bramble

# WebSocket (both boards on WiFi)
python3 scripts/e2e-test.py ws://192.0.2.0/ws ws://192.0.2.0/ws

# Serial (both boards via USB)
python3 scripts/e2e-test.py serial:/dev/ttyUSB0 serial:/dev/ttyUSB1

# Mixed
python3 scripts/e2e-test.py ws://192.0.2.0/ws serial:/dev/ttyUSB0
```

**Board addresses:**
- Board 1: `6E1EE666` — local VM `/dev/ttyUSB0`, WiFi `192.0.2.0`
- Board 2: `63929F02` — GPU box `/dev/ttyUSB0`, WiFi `192.0.2.0`

**When to run:**
- After flashing new firmware to boards
- After RPC method changes
- Before releases (validates real hardware behavior)

---

## Quick Decision Guide

| Changed... | Run... |
|------------|--------|
| `components/*.c` | Core unit tests (#1) |
| `bramble-go/*.go` | SDK tests (#2) |
| `webapp/src/*` | Webapp tests (#3) |
| Routing/mesh protocol | Core unit tests (#1) + simulator (#4) |
| RPC methods | Core unit tests (#1) + SDK tests (#2) + hardware E2E (#5) |
| Webapp transport layer | Webapp tests (#3) |
| Everything / pre-release | All of the above |

## Full Test Run (everything except hardware)

```bash
# 1. Core unit tests
cd /home/user/.local/workspace/bramble && bash test/run_all_tests.sh

# 2. Go SDK tests
cd /home/user/.local/workspace/bramble-go && go test ./...

# 3. Webapp tests
cd /home/user/.local/workspace/bramble/webapp && npx vitest run

# 4. Simulator smoke test
cd /home/user/.local/workspace/bramble/simulator && bash test-e2e.sh
```
