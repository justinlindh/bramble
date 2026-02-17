# Bramble Protocol Security Audit

**Audit Date:** 2026-02-17  
**Auditor:** Adversarial Security Review  
**Protocol Version:** 0.1-draft / 0.2 implementation  
**Scope:** Full protocol specification, C implementation, test coverage

---

## 1. Executive Summary

### Overall Security Rating: **B+**

Bramble demonstrates strong security fundamentals with thoughtful privacy-first design. The protocol substantially improves upon Meshtastic's security posture, but several gaps remain that warrant attention before production deployment.

**Justification:**
- Strong cryptographic foundations (X25519, AES-256-GCM, HKDF)
- Well-designed privacy protections for route discovery
- Comprehensive threat model with documented mitigations
- However: several medium-severity attack vectors remain exploitable

### Top 5 Most Critical Findings

| # | Finding | Severity | Section |
|---|---------|----------|---------|
| 1 | **Emergency beacon 4-byte HMAC cancel is forgeable** — birthday attack reduces brute force to ~65K attempts | High | §4.8 |
| 2 | **Time sync manipulation enables replay acceptance** — attacker with fake GPS can shift mesh time | High | §4.5 |
| 3 | **Group DM forward secrecy gap** — current epoch key compromise exposes future messages | Medium | §3.5 |
| 4 | **Public channel provides no confidentiality** — design choice, but attack surface for disinformation | Medium | §3.4 |
| 5 | **Beacon HMAC is only 4 bytes** — collision probability 1/2³² allows forged beacons | Medium | §4.7 |

### Comparison to Meshtastic Security Posture

| Property | Meshtastic | Bramble | Winner |
|----------|-----------|---------|--------|
| DM encryption | Shared PSK (all members can decrypt all DMs) | Per-pair X25519 session keys | **Bramble** |
| Source privacy | Plaintext in packet header | Encrypted in RREQ, hidden in channel msgs | **Bramble** |
| Relay node visibility | Full message content | Opaque ciphertext only | **Bramble** |
| Replay protection | None / weak | Packet ID dedup + timestamp window | **Bramble** |
| Forward secrecy | None | Epoch-based ratchet (partial) | **Bramble** |
| Backward secrecy | None | Epoch key deletion | **Bramble** |
| Route authentication | None | RREP HMAC (8-byte) | **Bramble** |
| Sybil resistance | None | RSSI clustering heuristic | **Bramble** |
| DoS mitigation | None | Airtime budget, rate limiting | **Bramble** |
| Mature implementation | Years of deployment | New, less battle-tested | Meshtastic |

**Summary:** Bramble represents a significant security improvement over Meshtastic in nearly every dimension. The primary concern is implementation maturity.

---

## 2. Threat Model

### 2.1 Attacker Capabilities Assumed

This audit assumes an attacker with:

| Capability | Description |
|------------|-------------|
| **Custom firmware** | Heltec V3 with modified firmware capable of transmitting arbitrary packets |
| **Protocol knowledge** | Complete understanding of Bramble spec (open source) |
| **SDR capability** | Software-defined radio for passive monitoring of all LoRa traffic |
| **Physical proximity** | Within radio range of the target mesh |
| **Mesh participation** | Can join mesh with valid node identity (permissionless) |
| **Resource constraints** | Cannot perform >2³² cryptographic operations in real-time |

**Not assumed:**
- Compromise of target node's private keys (endpoint compromise)
- Global passive adversary monitoring all mesh traffic simultaneously
- Physical access to target devices
- Unlimited computational resources (e.g., AES-256 brute force)

### 2.2 Assets Being Protected

| Asset | Confidentiality | Integrity | Availability |
|-------|-----------------|-----------|--------------|
| DM message content | Critical | Critical | Normal |
| Channel message content | High | High | Normal |
| User location (when shared) | Critical | High | Low |
| Node identity (address) | Medium | High | N/A |
| Network topology | Medium | Medium | Low |
| Route information | Low | High | High |
| Time synchronization | Low | Medium | High |

### 2.3 Trust Boundaries

```
┌────────────────────────────────────────────────────────────────┐
│                     UNTRUSTED RADIO MEDIUM                      │
│ ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌──────────┐  │
│ │  Node A  │────│  Relay   │────│  Relay   │────│  Node B  │  │
│ │ (sender) │    │   X      │    │   Y      │    │ (dest)   │  │
│ └──────────┘    └──────────┘    └──────────┘    └──────────┘  │
│                                                                 │
│ Trust boundary: Node A ←→ Node B (E2E encrypted)               │
│ X and Y see: dest_addr, packet_id, ciphertext (for DMs)        │
│ X and Y cannot see: source, content, app_type                  │
└────────────────────────────────────────────────────────────────┘
```

**Channel messages:** All channel members are within the trust boundary. Any member can read all messages.

---

## 3. Cryptographic Analysis

### 3.1 Key Exchange (X25519 + Static DH)

**Implementation:** `components/crypto/crypto.h`, `components/security/security.c`

**Protocol:**
```
Initiate: A → B: {eph_pub_A, long_term_pub_A}
Respond:  B → A: {eph_pub_B, long_term_pub_B, auth_tag}
Confirm:  A → B: {confirm_tag}

Session key derivation:
  ss1 = X25519(eph_priv_A, long_term_pub_B)  // Ephemeral-Static
  ss2 = X25519(long_term_priv_A, long_term_pub_B)  // Static-Static
  session_key = HKDF-SHA256(ss1 || ss2, "bramble-dm-v1", addr_pair)
```

**Assessment:** ✅ **Sound**

| Property | Status | Notes |
|----------|--------|-------|
| Forward secrecy | ✅ Partial | Ephemeral keys provide forward secrecy for each key exchange |
| Key separation | ✅ | Different HKDF info strings for different key types |
| Authentication | ✅ | Double-DH binds both identities; auth_tag confirms derivation |
| Replay protection | ✅ | Random key_id prevents replaying old exchanges |

**Known weaknesses:**
- Static-static DH secret is reused across sessions with same peer — compromise of both long-term keys reveals all historical shared secrets
- No identity hiding — long-term public keys are transmitted in the clear

**Recommendation:** Consider X3DH (Signal Protocol) for identity hiding if future versions require stronger anonymity.

### 3.2 Encryption (AES-256-GCM)

**Implementation:** `components/crypto/crypto.h`, line 21-23

**Parameters:**
- Key size: 256 bits (`BRAMBLE_KEY_SIZE = 32`)
- Nonce size: 96 bits (`BRAMBLE_NONCE_SIZE = 12`)
- Tag size: 128 bits (`BRAMBLE_TAG_SIZE = 16`)

**Assessment:** ✅ **Sound**

| Property | Status | Notes |
|----------|--------|-------|
| Algorithm choice | ✅ | AES-256-GCM is industry standard AEAD |
| Nonce uniqueness | ✅ | `nonce = src_addr(4) || counter(4) || random(4)` |
| Tag size | ✅ | Full 128-bit tag, no truncation |
| AAD usage | ⚠️ | AAD is NULL for channel messages — no binding to headers |

**Nonce management (critical):**

From `components/timesync/include/anti_replay.h`:
```c
uint32_t anti_replay_next_nonce(anti_replay_cache_t *cache);
```

The nonce counter is persisted to NVS and restored with a safety margin on reboot. This prevents nonce reuse after power loss — a critical property for GCM security.

**Potential issue:** The counter is 32-bit, allowing 2³² messages per session key before reuse. The 65,536-message rekey trigger (`channel_epoch_messages = 256` per channel, DM rekey at 2¹⁶) is well below this limit.

**Recommendation:** The current AAD=NULL for channel messages is acceptable since the inner plaintext contains channel_id, epoch, and src_addr. However, binding the nonce to the ciphertext header would provide defense-in-depth against cut-and-paste attacks.

### 3.3 Key Derivation (HKDF-SHA256)

**Implementation:** `crypto_hkdf_sha256()` in `components/crypto/`

**Usage patterns observed:**

| Use Case | Salt | IKM | Info |
|----------|------|-----|------|
| DM session key | "bramble-dm-v1" | ss1 ∥ ss2 | addr_pair |
| Channel key | "bramble-channel-v1" | SHA256(PSK) | channel_id |
| Channel epoch | "bramble-channel-epoch" | current_key | epoch (BE16) |
| Group key | FNV-1a based | sorted_members ∥ name | N/A |
| RREQ OTP | "bramble-rreq-v1" | shared_static | time_bucket ∥ salt |

**Assessment:** ✅ **Generally sound**

The HKDF usage follows RFC 5869 recommendations. Salt and info strings are distinct per use case, preventing key reuse across contexts.

**Finding (Low):** Group key derivation uses FNV-1a instead of HKDF:

From `components/group/include/group.h`:
```c
int group_derive_key(const char *name, const uint32_t *sorted_members, int count,
                     uint8_t *key_out, uint8_t *id_out);
```

FNV-1a is a non-cryptographic hash. While the sorted member list provides entropy, using HKDF-SHA256 would be more conservative.

### 3.4 Public Channel Crypto (Well-Known PSK)

**Implementation:** `components/channel/include/public_channel.h`

```c
#define BRAMBLE_PUBLIC_CHANNEL_PSK "bramble-default"
```

**Assessment:** ⚠️ **By design, provides NO confidentiality**

The public channel encrypts with a well-known key, meaning:
- Any node (or passive observer who knows the PSK) can decrypt all Channel 0 messages
- Encryption provides only code path consistency, not security

**Security implications:**
1. **Disinformation attacks:** Attacker can inject convincing-looking messages on the public channel
2. **Emergency beacon abuse:** Emergency messages relayed on public channel are readable by all
3. **Network reconnaissance:** Passive observer learns all public channel traffic

**Mitigation status:** The spec acknowledges this explicitly. Rate limiting (`BRAMBLE_PUBLIC_CHANNEL_RATE_LIMIT_MS = 30000`) reduces spam impact.

**Recommendation:** Add clear UI indication that public channel is NOT private. Consider optional public channel signing for message authenticity (author verification without confidentiality).

### 3.5 Group DM Key Derivation (BLAKE2s / FNV-1a)

**Implementation:** `components/group/include/group.h`

The spec describes BLAKE2s but the implementation header shows:
```c
int group_derive_key(const char *name, const uint32_t *sorted_members, int count,
                     uint8_t *key_out, uint8_t *id_out);
```

**Epoch rotation:**
```c
#define GROUP_EPOCH_ADVANCE_THRESHOLD 256  /* messages before epoch advance */
int group_advance_epoch(bramble_group_t *group);
```

**Assessment:** ⚠️ **Forward secrecy gap**

| Property | Status | Impact |
|----------|--------|--------|
| Backward secrecy | ✅ | Old epoch keys deleted |
| Forward secrecy (within epoch) | ❌ | Current key reveals all 256 messages in epoch |
| Member enumeration | ✅ | Members not visible to non-members |

**Attack scenario:**
1. Attacker compromises a group member's device
2. Attacker obtains current epoch key
3. Attacker can derive ALL future epoch keys via HKDF chain
4. All future group messages are compromised until key re-establishment

**Mitigation difficulty:** True forward secrecy requires interactive key exchange among all members — impractical over LoRa's bandwidth constraints.

**Recommendation:** 
1. Document this limitation clearly for users
2. Consider per-message key derivation: `msg_key = HKDF(epoch_key, msg_counter)` to limit exposure window
3. Add manual "rekey group" command for recovery after suspected compromise

### 3.6 Session Key Rotation

**Implementation:** Documented in spec §5.5

**Triggers:**
- Time-based: Every 24 hours
- Message-count: After 65,536 messages (DM) / 256 messages (channel)

**Assessment:** ✅ **Sound**

The rotation thresholds are conservative relative to nonce space (2³²). The 256-message channel epoch rotation is particularly good for limiting exposure.

---

## 4. Protocol-Level Attacks

### 4.1 Replay Attacks

| Attack | Severity | Description | Mitigation Status | Recommendation |
|--------|----------|-------------|-------------------|----------------|
| **Packet ID replay** | Low | Re-inject captured packet | ✅ Mitigated | Dedup buffer (256 entries, 60s window) |
| **Timestamp replay** | Low | Replay old packet with valid timestamp | ✅ Mitigated | ±30s window + packet ID dedup |
| **Cross-session replay** | Low | Replay from previous session | ✅ Mitigated | Session key rotation invalidates old ciphertext |
| **Nonce reuse after reboot** | Medium | Power cycle to reset nonce counter | ✅ Mitigated | NVS persistence + safety margin |

**Implementation reference:** `components/dedup/include/dedup.h`
```c
#define DEDUP_MAX_ENTRIES 256
#define DEDUP_EXPIRY_MS   60000
```

**Finding (Informational):** The dedup buffer is per-source-address but packet_id is only 32 bits. With 256 entries and random packet_id generation, false positive collisions are negligible (~1 in 2²⁴ per source per minute).

---

### 4.2 Denial of Service Attacks

#### 4.2.1 Radio Jamming

| Attack | Severity | Exploit | Impact | Mitigation |
|--------|----------|---------|--------|------------|
| Continuous carrier | Critical | Transmit unmodulated signal | Total mesh blackout | ❌ **None** (physical layer) |
| Selective jamming | High | Jam only specific packet types (detect preamble) | Targeted disruption | ❌ **None** (requires frequency hopping) |

**Recommendation:** Future work should explore frequency hopping or spread spectrum techniques. Current single-channel operation is inherently vulnerable.

#### 4.2.2 Beacon Flooding

| Attack | Severity | Exploit | Impact | Mitigation |
|--------|----------|---------|--------|------------|
| Fake beacon storm | Medium | Inject high-rate beacons from fake identities | Neighbor table pollution, processing overhead | ⚠️ Partial |

**Mitigation analysis:**
- Per-source beacon rate limiting: 1 beacon/30s per source (`beacon_interval_s = 60` default)
- Sybil detection heuristic (RSSI clustering) — from `components/security/include/security.h`:
```c
#define SYBIL_RSSI_CLUSTER_THRESHOLD 3
#define SYBIL_MIN_SUSPECTS 3
```

**Weakness:** Attacker with multiple physical radios (or SDR) can defeat RSSI clustering by ensuring different received signal strengths.

**Exploit outline:**
```
1. Acquire 3 Heltec V3 devices at different locations
2. Generate 50 fake identities (fast: ~1 second total)
3. Distribute identities across devices
4. Each device beacons for its assigned identities
5. RSSI clustering fails (different physical locations)
6. Victim's neighbor table fills with fake entries
7. Route discovery returns attacker-controlled paths
```

**Recommendation:** Add beacon authentication requirement — unauthenticated beacons from unknown sources should be weighted lower for routing decisions (partially implemented via `auth_hmac` field).

#### 4.2.3 RREQ Flooding

| Attack | Severity | Exploit | Impact | Mitigation |
|--------|----------|---------|--------|------------|
| Route discovery storm | Medium | Generate RREQ for non-existent destinations | Airtime exhaustion, processing load | ✅ Mitigated |

**Mitigation:** `components/security/include/security.h`
```c
#define RREQ_RATE_LIMIT_MS 30000  // 30s between RREQs for same (neighbor, dest) pair
#define RREQ_RATE_ENTRIES 64
```

**Analysis:** Rate limiting is per-(neighbor, destination) pair. An attacker can still generate RREQs for 64 different destinations at 1/30s each = ~2 RREQ/second aggregate. This is acceptable but not complete protection.

**Recommendation:** Add global RREQ budget per neighbor (e.g., max 10 RREQs/minute regardless of destination).

#### 4.2.4 Airtime Exhaustion

| Attack | Severity | Exploit | Impact | Mitigation |
|--------|----------|---------|--------|------------|
| Self-exhaustion | Low | Malicious node exhausts own airtime budget | Attacker becomes silent | ✅ By design (self-limiting) |
| Trigger relay exhaustion | Medium | Send many packets requiring relay by victim | Victim's airtime budget depleted | ⚠️ Partial |

**Implementation:** `components/airtime/include/airtime_budget.h`
```c
#define AIRTIME_BUDGET_CRITICAL_MS   36000  // 36s/hour for critical
#define AIRTIME_BUDGET_NORMAL_MS     18000  // 18s/hour for normal
#define AIRTIME_BUDGET_BROADCAST_MS  18000  // 18s/hour for broadcast
```

**Weakness:** A node that relays packets for others consumes its own airtime budget. An attacker can route traffic through a victim to exhaust the victim's budget.

**Exploit outline:**
```
1. Identify target node V in the mesh
2. Discover routes where V is on the path
3. Generate high volume of Critical-tier traffic through V
4. V's airtime budget depletes (Critical can borrow from Normal and Broadcast)
5. V cannot transmit its own traffic
```

**Mitigation status:** Emergency packets bypass airtime checks (`HEADER_FLAG_EMERGENCY`), ensuring emergency beacons still work even under budget exhaustion.

**Recommendation:** 
1. Track per-source relay airtime separately from local origination
2. Implement "fair queuing" — don't relay excessive traffic from any single source
3. Consider charging the source node's budget for relayed traffic (requires distributed accounting)

---

### 4.3 Sybil Attacks

| Attack | Severity | Exploit | Impact | Mitigation |
|--------|----------|---------|--------|------------|
| Fake identity creation | Medium | Generate many X25519 keypairs | Route manipulation, topology poisoning | ⚠️ Partial (RSSI heuristic) |

**Implementation:** `sybil_check_rssi_cluster()` in `components/security/security.c`

**Analysis:** The Sybil defense relies on:
1. **Airtime cost:** Each identity must beacon separately (10% duty cycle limits ~3-4 active identities per radio)
2. **RSSI clustering:** Multiple identities from one physical radio have similar RSSI

**Weaknesses:**
- Attacker with multiple radios defeats both defenses
- Keypair generation is computationally trivial (~50/sec on ESP32)
- No proof-of-work or stake mechanism

**Recommendation:** Consider optional "trusted introducer" mode where new nodes require endorsement from existing trusted nodes. This trades openness for Sybil resistance.

---

### 4.4 Eclipse Attacks

| Attack | Severity | Exploit | Impact | Mitigation |
|--------|----------|---------|--------|------------|
| Isolate target node | High | Surround victim with Sybil nodes | All victim traffic routed through attacker | ⚠️ Partial |

**Exploit outline:**
```
1. Deploy multiple attacker nodes around victim V
2. Attacker nodes have better RSSI to V than legitimate neighbors
3. Attacker nodes claim routes to all destinations
4. V's neighbor table fills with attacker entries
5. V's routing table points to attacker-controlled next-hops
6. All V's traffic is intercepted (can't decrypt DMs but can:)
   - Drop packets (DoS)
   - Delay packets
   - Traffic analysis
   - Replay packets
```

**Mitigation status:**
- RREP authentication (`auth_hmac[8]`) prevents forged route replies from authenticated peers
- BUT: First-contact mode (`OPEN_SOURCE` flag) accepts unauthenticated routes

**Recommendation:**
1. Prefer routes through previously-authenticated peers
2. Maintain "anchor" neighbors that are rarely evicted
3. Add route diversity — use multiple paths when available

---

### 4.5 Time Sync Manipulation

| Attack | Severity | Exploit | Impact | Mitigation |
|--------|----------|---------|--------|------------|
| Fake GPS stratum-0 | High | Claim to be GPS-synced with false time | Mesh time shifts, replay window manipulation | ⚠️ Partial |

**Implementation:** `components/timesync/include/timesync.h`

**Mitigation:** Stratum-0 corroboration requirement (spec §9.3):
```
Stratum-0 claims require at least 2 neighbors to report consistent
stratum-0 derived time within 2 beacon cycles before adoption
```

**Weaknesses:**
1. If attacker controls 2+ nodes near victim, corroboration succeeds
2. ±5 second clamp per sync interval allows gradual drift manipulation

**Attack scenario:**
```
1. Attacker deploys 3 nodes near victim
2. All claim stratum-0 (fake GPS) with time +25 seconds
3. Victim requires 2 corroborators → satisfied
4. Victim's time shifts +5s per sync (clamped)
5. After 5 sync intervals (~25 minutes), victim is +25s off
6. Attacker captures packets at real time T
7. Attacker replays at T+25s → passes victim's ±30s window
```

**Recommendation:**
1. Increase corroboration requirement to 3+ neighbors for stratum-0
2. Implement time source reputation — suspicious sources weighted lower
3. Consider GPS receiver authentication (signed NMEA) for critical deployments

---

### 4.6 Traffic Analysis

| Attack | Severity | Exploit | Impact | Mitigation |
|--------|----------|---------|--------|------------|
| Volume correlation | Medium | Observe tx/rx patterns between nodes | Identify communication pairs | ⚠️ Partial (dummy traffic) |
| Timing correlation | Medium | Measure send-to-receive latency | Map route paths | ⚠️ Partial |
| Packet size fingerprinting | Low | Distinguish message types by size | Learn traffic patterns | ❌ Limited |

**Mitigation: Dummy traffic** (`components/security/include/dummy_traffic.h`)
```c
#define DUMMY_TRAFFIC_MIN_INTERVAL_MS  5000
#define DUMMY_TRAFFIC_MAX_INTERVAL_MS  30000
#define DUMMY_TRAFFIC_AIRTIME_BUDGET_PCT 2
```

**Analysis:** Dummy traffic adds 2% airtime overhead generating fake encrypted packets. This provides:
- Volume obfuscation (constant traffic even when silent)
- Size randomization (`DUMMY_TRAFFIC_MIN_SIZE = 20` to `DUMMY_TRAFFIC_MAX_SIZE = 120`)

**Weaknesses:**
1. Dummy traffic is optional (`enabled` flag) — not on by default
2. 2% budget is low — sophisticated attacker can statistically filter
3. Timing patterns still leak (dummy has different retry patterns than real traffic)

**Recommendation:**
1. Enable dummy traffic by default in privacy-sensitive deployments
2. Match dummy retry patterns to real traffic
3. Consider constant-rate transmission mode (pad all time slots)

---

### 4.7 Beacon Spoofing

| Attack | Severity | Exploit | Impact | Mitigation |
|--------|----------|---------|--------|------------|
| Forge beacon from known peer | Medium | Brute-force 4-byte HMAC | Impersonate trusted node | ⚠️ Weak HMAC |

**Implementation:** `components/packet/include/packet.h`
```c
typedef struct {
    // ...
    uint8_t  auth_hmac[4];  // 4-byte truncated HMAC-SHA256
} bramble_beacon_t;
```

**Analysis:** The beacon HMAC is truncated to 32 bits. Birthday attack considerations:
- Brute force: 2³² attempts (~4 billion) — feasible with dedicated hardware
- Per-packet verification: ~0.1ms on ESP32 — 4 billion attempts = ~4.6 days on single ESP32
- With 100 parallel attackers: ~1 hour

**Exploit scenario:**
1. Capture legitimate beacon from target T
2. Modify battery_pct, tx_queue_depth, network_time
3. Brute-force auth_hmac (known key if attacker is peer, unknown if not)
4. If attacker has session key: instant forgery
5. Inject forged beacon claiming T is congested → traffic reroutes around T

**Recommendation:** Increase beacon HMAC to 8 bytes (64 bits) — 2⁶⁴ brute force is infeasible.

---

### 4.8 Emergency Beacon Abuse

| Attack | Severity | Exploit | Impact | Mitigation |
|--------|----------|---------|--------|------------|
| False distress signal | Medium | Broadcast fake emergency | Waste responder resources | ⚠️ Social accountability only |
| Cancel victim's emergency | High | Forge EMERGENCY_CANCEL | Suppress legitimate distress | ⚠️ Weak (4-byte auth) |

**Implementation:** `components/emergency/include/emergency.h`
```c
typedef struct {
    uint32_t src_addr;
    uint32_t cancel_timestamp;
    uint8_t auth_tag[4];  /* HMAC-SHA256 truncated to 4 bytes */
} emergency_cancel_t;
```

**Critical finding:** EMERGENCY_CANCEL uses only 4-byte (32-bit) authentication.

**Attack: Forge emergency cancellation**
```
1. Observe victim V broadcasting EMERGENCY beacon
2. Note V's src_addr and cancel_timestamp range
3. Brute-force 4-byte auth_tag (if attacker has session key: instant)
4. Without session key: birthday attack ~2^16 = 65,536 attempts
   - At 1000 attempts/second = ~65 seconds
5. Broadcast forged EMERGENCY_CANCEL
6. Victim's legitimate emergency is suppressed
```

**Severity justification:** This is HIGH severity because:
- Human safety implications — a forged cancel could leave someone in danger
- Asymmetric risk profile (false emergency wastes resources; false cancel may cost lives)
- The 4-byte HMAC was chosen to save packet size, but the cost/benefit is wrong

**Recommendation (Critical):** 
1. Increase EMERGENCY_CANCEL auth_tag to at least 8 bytes
2. Consider requiring multiple cancel packets (N-of-M consensus)
3. Add cancel rate limiting — only 1 cancel per emergency activation

---

### 4.9 Mailbox Poisoning

| Attack | Severity | Exploit | Impact | Mitigation |
|--------|----------|---------|--------|------------|
| Fill mailbox with garbage | Medium | Store messages for many fake destinations | Legitimate messages evicted | ✅ Mitigated |

**Implementation:** `components/mailbox/include/mailbox.h`
```c
#define MAILBOX_MAX_ENTRIES    32
#define MAILBOX_MAX_PER_DEST   8
#define MAILBOX_MAX_PER_SOURCE 8
```

**Mitigation analysis:**
- Per-source cap (8 messages) limits attacker to 8 entries per identity
- Per-destination cap prevents targeting single victim
- HMAC authentication required for STORE_REQUEST

**Residual risk:** Attacker with multiple identities can still fill mailbox (32 / 8 = 4 identities needed).

**Recommendation:** Add priority for messages from established peers over unknown sources.

---

### 4.10 RREQ/RREP Manipulation

| Attack | Severity | Exploit | Impact | Mitigation |
|--------|----------|---------|--------|------------|
| Forge RREP from unknown peer | Medium | Send unauthenticated RREP | Install attacker-controlled route | ⚠️ Partial |
| Metric manipulation | Low | Relay RREQ with inflated metric | Attract traffic through attacker | ✅ Bounded |

**RREP authentication:** `components/packet/include/packet.h`
```c
typedef struct {
    // ...
    uint8_t  auth_hmac[8];  // 8-byte truncated HMAC
} bramble_rrep_t;
```

**Analysis:**
- Authenticated RREPs (from known peers) are verified
- First-contact RREPs (`auth_hmac = 0x0000000000000000`) create "unverified" routes
- Unverified routes are only promoted after successful KEY_EXCHANGE

**Attack scenario (first-contact):**
```
1. Node A initiates RREQ for unknown node B (OPEN_SOURCE mode)
2. Attacker M intercepts, sends fake RREP claiming to be B
3. A installs unverified route to B via M
4. A sends KEY_EXCHANGE to B via M
5. M relays KEY_EXCHANGE to real B
6. B responds, M relays back
7. Route promoted — M is now MITM
```

**Mitigation:** The KEY_EXCHANGE is end-to-end encrypted with ephemeral keys. M cannot inject or modify the exchange without detection.

**Residual risk:** M can perform traffic analysis and selective dropping even after KEY_EXCHANGE completes.

**Recommendation:** 
1. Warn users that first-contact routes are unverified
2. Display route path to user for manual verification
3. Allow user to pin trusted routes

---

### 4.11 Key Compromise Propagation

| Attack | Severity | Exploit | Impact | Mitigation |
|--------|----------|---------|--------|------------|
| Compromise one node | Variable | Extract keys from device | Access to that node's sessions | ✅ Contained |
| Static key compromise | High | Extract long-term private key | Derive all past static-DH secrets | ⚠️ No PFS for static-static |

**Containment analysis:**
- DM session keys: Per-pair, so compromise of A's key reveals A's DMs only
- Channel keys: Compromise reveals that channel's content
- Group keys: Compromise reveals group content (and future via epoch derivation)
- Long-term key: Reveals identity, enables impersonation, exposes static-DH secrets

**Recommendation:**
1. Implement key zeroization on tamper detection (requires hardware support)
2. Document recovery procedures (rotate channel PSKs, revoke node)
3. Consider ephemeral-only mode (no static-static DH) for highest security

---

### 4.12 Downgrade Attacks

| Attack | Severity | Exploit | Impact | Mitigation |
|--------|----------|---------|--------|------------|
| Force OPEN_SOURCE mode | Medium | Block encrypted RREQ responses | Privacy degradation | ⚠️ Partial |
| Protocol version rollback | Low | Claim older protocol version | Exploit old vulnerabilities | ✅ Version field validated |

**Analysis:** 
- `allow_open_rreq` config flag controls OPEN_SOURCE mode (default: false)
- Attacker cannot force mode change remotely
- Version field is checked on deserialization

**Residual risk:** If user enables OPEN_SOURCE mode for convenience, privacy degrades.

**Recommendation:** Clear UI warning when OPEN_SOURCE mode is enabled.

---

## 5. Implementation-Specific Findings

### 5.1 Buffer Overflow Potential

| File | Location | Finding | Severity | Status |
|------|----------|---------|----------|--------|
| `packet.c` | All deserialize functions | Length checks before access | ✅ | Safe |
| `channel_msg.c:encrypt` | Line ~15 | Stack buffer `pt[2048]` | ⚠️ | See below |
| `mailbox.c:store` | Line ~50 | `len > MAILBOX_MAX_PAYLOAD` check | ✅ | Safe |
| `fragment.h` | `FRAG_MAX_PLAINTEXT = 154` | Bounds enforced | ✅ | Safe |

**Finding (Medium):** `channel_msg.c` uses large stack buffer:
```c
int channel_msg_encrypt(...) {
    uint8_t pt[2048];  // On stack
    if (pt_len > sizeof(pt)) return -1;
```

**Risk:** Stack overflow if function is called in deep call chain. ESP32-S3 tasks typically have 4KB stacks.

**Recommendation:** Use static or heap allocation for large buffers, or reduce buffer size (max channel payload is ~173 bytes).

### 5.2 Integer Overflow in Metric Calculations

**File:** `components/routing/route_metric.c`

Examining the composite metric calculation:
```c
uint8_t route_metric_compute(uint8_t link_quality,
                             uint8_t delivery_rate,
                             uint8_t airtime_remaining,
                             uint16_t latency_ms);
```

**Expected calculation:**
```c
raw = (102 * link + 77 * delivery + 51 * airtime + 26 * latency) / 256;
```

**Analysis:** Maximum value:
- `102 * 255 + 77 * 255 + 51 * 255 + 26 * 255 = 65,280`
- Fits in uint16_t (max 65,535) ✅

**Status:** Safe — no overflow possible with uint8_t inputs.

### 5.3 Memory Safety in C Code

| Pattern | Status | Notes |
|---------|--------|-------|
| Static allocation | ✅ | All major data structures pre-allocated |
| No malloc after init | ✅ | Follows embedded best practices |
| Bounds checking | ✅ | All array accesses check bounds |
| Null pointer checks | ✅ | Input validation on public functions |
| Memset before use | ✅ | Structures zeroed on init |

**Finding (Low):** Several functions don't check for NULL before memcpy:
```c
// discovery.c
bramble_rreq_t rreq_forward(const bramble_rreq_t *incoming, ...) {
    bramble_rreq_t r = *incoming;  // No NULL check
```

**Impact:** Low — internal functions, callers are responsible.

### 5.4 Race Conditions

**Architecture:** Bramble runs on FreeRTOS with multiple tasks:
- Radio task (TX/RX)
- Protocol task (routing, reliability)
- Application task (UI)

**Shared state:**
- Routing table
- Neighbor table
- TX queue
- Key cache

**Analysis:** Without seeing the full integration code, potential races exist at:
- Route table updates during active forwarding
- Key cache access during encryption/decryption

**Recommendation:** 
1. Use FreeRTOS mutexes for shared data structures
2. Consider message-passing architecture (queues between tasks)

### 5.5 Error Handling Gaps

| File | Function | Issue | Recommendation |
|------|----------|-------|----------------|
| `channel_msg.c` | `channel_msg_decrypt` | Returns -1 for all failures | Add error codes |
| `mailbox.c` | `mailbox_store` | Returns -1 or -2 | Document error codes |
| `discovery.c` | `discovery_start` | Returns -1 if full | No retry mechanism |

**General observation:** Error codes are inconsistent. Some functions use ESP_ERR_* codes, others use -1/0.

**Recommendation:** Standardize on ESP-IDF error codes throughout.

---

## 6. Privacy Analysis

### 6.1 Metadata Leakage

| Field | Visible to | Privacy Impact |
|-------|------------|----------------|
| `dest_addr` (DM) | All relay nodes | Reveals communication target |
| `src_addr` (DM) | Hidden (in header), only destination knows | ✅ Good |
| `src_addr` (Channel) | Hidden (encrypted in payload) | ✅ Good |
| `packet_id` | All observers | Can track packet through mesh |
| `hop_limit` | All relay nodes | Reveals distance from source |
| `packet size` | All observers | Can fingerprint message type |

**Attack: Destination tracking**

An observer who sees `dest_addr = 0xDEADBEEF` on multiple packets can:
1. Determine that node 0xDEADBEEF is receiving messages
2. Correlate timing to identify communication patterns
3. Track node if it moves (address is persistent)

**Mitigation status:** Address rotation is listed as future work (spec §13.2).

**Recommendation (High):** Implement optional destination address encryption:
```
encrypted_dest = dest_addr XOR HKDF(routing_key, "dest-mask")
```
Relay nodes use a different mechanism (e.g., onion routing) to forward.

### 6.2 Beacon Fingerprinting

**Observable beacon fields:**
- `src_addr` — node identity
- `pubkey_hash` — unique per node
- `uptime_min` — device characteristic
- `battery_pct` — power profile
- `tx_queue_depth` — activity level
- `network_time` / `time_confidence` — sync status

**Attack: Node profiling**

Over time, an observer can build profiles:
- "Node 0xABCD has high uptime, usually 90%+ battery — likely fixed installation"
- "Node 0x1234 has fluctuating battery, short uptime — likely mobile user"

**Recommendation:**
1. Add jitter to uptime reporting (±5 minutes)
2. Quantize battery to ranges (0-25%, 25-50%, 50-75%, 75-100%)
3. Consider randomized beacon timing (already has ±10s jitter)

### 6.3 Traffic Correlation

**Attack: End-to-end correlation**

```
1. Observer at location L1 sees packet P (by packet_id)
2. Observer at location L2 sees same packet P forwarded
3. Correlation: Source is near L1, destination is beyond L2
4. With enough observation points: full path reconstruction
```

**Mitigation status:** Packet IDs are random but persistent through the path.

**Recommendation:** Consider per-hop packet ID transformation:
```
new_packet_id = HMAC(relay_key, old_packet_id)[0:4]
```
This breaks correlation while maintaining dedup functionality locally.

### 6.4 Location Tier Enforcement

**Implementation:** `components/location/include/location.h`

| Tier | Data Shared | Coarse Resolution |
|------|-------------|-------------------|
| FULL | lat, lon, alt, speed, heading | ~1m (GPS) |
| COARSE | grid_lat, grid_lon | ~1 km |
| PRESENCE | online/offline | None |

**Analysis:** The tier system is enforced client-side. A malicious recipient cannot extract finer location from coarse data (it's truly quantized, not just rounded).

**Finding (Medium):** COARSE tier uses 0.01° quantization:
- 0.01° latitude ≈ 1.11 km
- 0.01° longitude ≈ 0.85 km at 40° latitude (varies with latitude)

**Potential attack:** If attacker receives multiple COARSE updates over time as target moves, they can reconstruct the path at 1km resolution.

**Recommendation:** Add temporal quantization — don't send updates more than once per N minutes at COARSE tier.

### 6.5 Dummy Traffic Effectiveness

**Configuration:**
```c
#define DUMMY_TRAFFIC_MIN_INTERVAL_MS  5000   // 5 seconds
#define DUMMY_TRAFFIC_MAX_INTERVAL_MS  30000  // 30 seconds
#define DUMMY_TRAFFIC_AIRTIME_BUDGET_PCT 2    // 2% of airtime
```

**Statistical analysis:**

At 2% airtime budget with ~500ms average packet time:
- ~4 dummy packets per hour
- Real message rate for active user: ~10-50 per hour
- Ratio: 8-100% dummy traffic

**Effectiveness:** Moderate. Sophisticated attacker can use:
- Timing analysis (dummy has no reply, real messages get ACKs)
- Volume analysis (filter out constant-rate dummy)
- Behavioral analysis (dummy is independent of real activity patterns)

**Recommendation:** 
1. Generate dummy ACKs for dummy packets (with probability)
2. Correlate dummy rate with real activity (more dummies when sending real traffic)
3. Consider constant-rate mode for high-security scenarios

---

## 7. Recommendations

### 7.1 Critical Priority (Fix Before Deployment)

| # | Finding | Recommendation | Effort |
|---|---------|----------------|--------|
| 1 | Emergency cancel 4-byte auth | Increase to 8 bytes | Low |
| 2 | Beacon HMAC 4 bytes | Increase to 8 bytes | Low |
| 3 | Stack buffer in channel_msg.c | Use static allocation | Low |

### 7.2 High Priority (Fix in Next Release)

| # | Finding | Recommendation | Effort |
|---|---------|----------------|--------|
| 4 | Time sync manipulation | Require 3+ corroborators for stratum-0 | Medium |
| 5 | Group DM forward secrecy | Per-message key derivation | Medium |
| 6 | Relay airtime exhaustion | Per-source relay accounting | Medium |
| 7 | First-contact route MITM | UI warning for unverified routes | Low |

### 7.3 Medium Priority (Address in Roadmap)

| # | Finding | Recommendation | Effort |
|---|---------|----------------|--------|
| 8 | Destination tracking | Address rotation | High |
| 9 | Packet ID correlation | Per-hop ID transformation | High |
| 10 | Eclipse attack | Anchor neighbors, route diversity | High |
| 11 | Beacon fingerprinting | Quantize/jitter observable fields | Medium |
| 12 | Group key FNV-1a | Switch to HKDF | Low |

### 7.4 Quick Wins (Low Effort, High Value)

1. **Error code standardization** — use ESP_ERR_* consistently
2. **Security documentation** — user-facing threat model summary
3. **Public channel warning** — clear UI indication of no confidentiality
4. **OPEN_SOURCE mode warning** — alert when privacy-reduced mode active
5. **Test coverage expansion** — see §8

---

## 8. Test Coverage Gaps

### 8.1 Current Test Coverage

| Component | Test File | Coverage Level |
|-----------|-----------|----------------|
| Packet serialization | `test_packet.c` | ✅ Good |
| Crypto primitives | `test_crypto.c`, `test_crypto_vectors.c` | ✅ Good |
| Channel encryption | `test_channel_msg.c` | ✅ Good |
| RREQ privacy | `test_rreq_privacy.c` | ✅ Good |
| Rate limiting | `test_security.c` | ⚠️ Minimal |
| Anti-replay | `test_anti_replay.c` | ✅ Good |
| Routing | `test_routing.c`, `test_discovery.c` | ⚠️ Moderate |
| Emergency | `test_emergency.c` | ✅ Good |
| Mailbox | `test_mailbox.c` | ⚠️ Minimal |

### 8.2 Missing Adversarial Test Cases

| Attack Scenario | Test Status | Suggested Test |
|-----------------|-------------|----------------|
| RREQ flooding (rate limit) | ⚠️ Partial | Exceed rate limit, verify rejection |
| Beacon flooding from Sybil | ❌ Missing | 10+ beacons from clustered RSSI, verify detection |
| Forged RREP injection | ❌ Missing | Inject RREP with invalid HMAC, verify rejection |
| Emergency cancel forgery | ❌ Missing | Test auth_tag verification, brute force resistance |
| Time sync manipulation | ❌ Missing | Inject false stratum-0, verify corroboration |
| Mailbox overflow | ⚠️ Partial | Fill all slots, verify FIFO eviction |
| Replay within window | ⚠️ Partial | Replay packet at +29s, verify rejection |
| Nonce counter wrap | ❌ Missing | Simulate 2³² messages, verify rekey |
| Cross-session replay | ❌ Missing | Save ciphertext, rekey, attempt replay |
| Fragment reassembly attack | ❌ Missing | Mix fragments from different messages |

### 8.3 Suggested Adversarial Test Suite

```c
// test_adversarial.c — suggested additions

void test_beacon_flood_sybil_detection(void) {
    // Generate 10 beacons from same physical location (similar RSSI)
    // Verify RSSI clustering triggers suspicious flag
}

void test_rrep_invalid_hmac_rejected(void) {
    // Build RREP with known session key
    // Corrupt auth_hmac
    // Verify rejection at origin
}

void test_emergency_cancel_requires_auth(void) {
    // Activate emergency for node A
    // Attempt cancel with wrong auth_tag
    // Verify emergency still active
}

void test_time_sync_corroboration_required(void) {
    // Single stratum-0 claim
    // Verify NOT adopted without corroboration
    // Add second corroborator
    // Verify adopted
}

void test_fragment_mix_attack(void) {
    // Send frag 0 of message A
    // Send frag 1 of message B (same message_id)
    // Verify auth tag fails / rejection
}

void test_replay_boundary_conditions(void) {
    // Packet at time T
    // Attempt replay at T + 29s (should reject)
    // Attempt replay at T + 31s (should reject)
    // New packet at T + 60s (should accept)
}
```

---

## Appendix A: Severity Ratings

| Rating | Definition |
|--------|------------|
| **Critical** | Immediate exploitation leads to complete compromise of confidentiality, integrity, or availability |
| **High** | Exploitation requires moderate effort but leads to significant impact; human safety implications |
| **Medium** | Exploitation possible under specific conditions; impact is contained |
| **Low** | Minor issue; exploitation difficult or impact minimal |
| **Informational** | Best practice deviation; no direct security impact |

---

## Appendix B: References

- Bramble Protocol Specification v0.1-draft (2026-02-17)
- Bramble Architecture Documentation
- Source code: `components/` directory
- Test suite: `test/` directory
- RFC 5869: HMAC-based Extract-and-Expand Key Derivation Function (HKDF)
- RFC 7748: Elliptic Curves for Security (X25519)
- RFC 5116: An Interface and Algorithms for Authenticated Encryption (GCM)
- Meshtastic Protocol Documentation (comparison reference)

---

**Document History:**
| Date | Version | Changes |
|------|---------|---------|
| 2026-02-17 | 1.0 | Initial security audit |
