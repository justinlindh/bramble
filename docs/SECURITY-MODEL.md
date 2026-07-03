# Bramble Security Model

This document is the threat model for Bramble and an honest statement of the
current security posture. It describes what the code on `main` does today,
verified against the source files cited throughout. It does not describe
intentions. When the implementation changes, this document must change in the
same PR.

A protection is only listed here if the cited code implements it. A gap is
listed as a fact of the present code, not as a roadmap item. If you find a
claim in this document that the code does not back, that is a bug in this
document; file it.

## 1. Adversary classes

### Classes the design intends to defend against

**Passive RF observer.** Someone in radio range with an SDR or a stock SX1262
board running modified firmware, logging every LoRa frame on Bramble's
frequencies. They capture all traffic but transmit nothing. Defended against
for message *content* on configured private channels (AES-256-GCM, section 3).
Not defended against for most *metadata* and, today, not for location packets
(sections 4 and 5).

**Active RF injector.** The same attacker, now transmitting: forging packets,
replaying captured frames, jamming. Partially defended against today.
Encrypted payloads cannot be forged without a channel key, and the cleartext
header is bound to the ciphertext as AEAD associated data. Location packets
are now encrypted end to end (section 3), and routing control traffic
(RREP, RERR, beacon, ACK, delivery receipt) now carries a network-key HMAC,
but that key is a public fallback until provisioned, and even provisioned it
does not stop replay of a captured, genuinely-valid control message
(section 4, section 5): this attacker can still forge routing control
traffic against an unprovisioned fleet, and can replay it against any
fleet. Mailbox triggers ride the same staged beacon key.
Jamming is not defendable at this layer and is accepted (section 5).

**Malicious mesh member.** A node that legitimately holds one or more channel
keys: an invited member gone bad, or a stolen key. They decrypt everything on
those channels, can impersonate any source address inside those channels, and
participate fully in routing. The design goal is to limit them to the
channels they hold keys for and to make routing lies detectable. A member
who also holds the network key can still forge or replay any routing
control message on behalf of any other member (section 5): authentication
proves "signed by a network-key holder", not "signed by this specific
member", so routing lies from a key-holding insider remain undetectable by
design, not by omission.

**Mailbox or relay operator.** Any node forwards packets, and a node with
mailbox mode enabled stores ciphertext for offline peers. The design intends
relays and mailboxes to learn nothing beyond ciphertext, sizes, timing, and
the cleartext header fields. That holds for channel messages today; it does
not hold for location packets, which a relay reads in cleartext (section 4).

**Device thief with flash access.** Someone with the physical device, or just
its flash chip, and standard ESP32 tooling (`esptool.py`, NVS partition
parsers). Today this adversary wins completely: identity private key, channel
keys, the RPC auth token, and stored message history are all readable from
flash (section 4). The design intends flash encryption to defeat this class;
it is not enabled today.

**Network-adjacent attacker.** Three sub-cases:

- *Same Wi-Fi network as the node.* The RPC interface (WebSocket on port 80,
  `main/ws_server.c`) requires a per-device bearer token that the firmware
  generates on first boot (`identity_ensure_ws_auth_token` in
  `components/identity/identity.c`). Unauthenticated connections are limited
  to a two-method identification allowlist (`components/rpc/rpc_auth.c`) and
  receive no server-push notifications (message content, GPS, peer
  locations are pushed only to authenticated connections,
  `rpc_auth_notify_filter`). The transport is still plaintext HTTP, so an
  on-path attacker who captures a legitimate session reads everything,
  token included (section 4).
- *Malicious web page in a browser on the LAN.* Connections that do not
  present the valid token are subject to an `Origin` check: same-origin
  (the host the request was addressed to) and an allowlist managed via
  authenticated RPC pass; everything else, including the literal `null`
  origin, is rejected with a 1008 close frame (`main/ws_origin.c`,
  enforced in `main/ws_server.c`). A connection that presents the valid
  token skips the Origin check: a cross-site page cannot read the token,
  so a token-bearing client is the user's own, not hijacked. Non-browser
  clients send no Origin and are not subject to cross-site WebSocket
  hijacking.
- *BLE proximity.* The BLE RPC transport gates on the same default-on token
  as WS, with a first-write handshake and throttled retries
  (`components/ble/ble_server.c`); pre-handshake JSON-RPC is limited to the
  same identification allowlist, and dispatcher notifications are withheld
  until the handshake succeeds.

**Compromised OTA source.** An attacker who controls the firmware download
server, the URL given to the device, or the TLS path. Three controls stack
here: HTTPS with certificate bundle validation (`components/ota/ota.c`), an
origin allowlist (the device only fetches from its NVS-configured OTA origin;
`bramble.otaUpdate` takes a relative artifact path, never a raw URL;
`components/ota/ota_url.c`, `ota_origin.c`), and mandatory image signature
verification (RSA-3072 Secure Boot V2 app signatures, checked on every OTA
write via `CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT`). On a device
already running signed firmware, a compromised server or MITM cannot install
firmware without the signing key; devices still running pre-signing firmware
perform no check and remain exposed until they take their first signed OTA.
A soft
anti-rollback floor (NVS-stored, `components/ota/ota_rollback.c`) rejects
downgrades unless a token holder explicitly overrides. Residual: signature
enforcement happens at OTA time, not at boot, until hardware Secure Boot V2
is burned (section 4).

### Out of scope

These are accepted as outside what Bramble can or will defend against:

- **RF direction finding and triangulation.** Any transmitter can be located
  by an adversary with directional antennas or multiple receivers. This is
  physics. If you transmit, your radio can be found.
- **Global passive observer.** An adversary that records all RF everywhere
  defeats any mesh's traffic analysis resistance. Bramble does not attempt
  cover traffic at the scale needed to resist this.
- **Transmission presence.** Even a single in-range observer learns that *a*
  Bramble node transmitted at a given time, with a given size, on a given
  frequency. Frame existence is not concealable on a broadcast medium.
- **Jamming and RF denial of service.** LoRa is resilient to narrowband
  interference but not to a deliberate wideband jammer.
- **Compromised end device while running.** If the attacker has code
  execution on a node, or on the phone or laptop running the web app, the
  content that device can see is theirs.
- **Side-channel extraction from a running, physically-held device** (power
  analysis, fault injection). Plaintext flash dumping is in scope (and
  currently unmitigated); lab-grade attacks beyond that are not.

## 2. Assets, ranked

1. **Identity keys.** The X25519 private key is the root of a node's
   identity. Theft enables permanent impersonation and unlocks everything
   derived from it. Highest value, longest lifetime.
2. **Message content.** What was said. The core promise of the project.
3. **Location.** Where a person is or was. Treated at the same severity as
   message content; arguably worse, because it maps to physical safety.
4. **Channel membership and channel keys.** Holding a channel passphrase
   means reading every message ever sent on that channel, past captures
   included: keys derive deterministically from the passphrase and each epoch
   key derives from the previous one
   (`components/channel/channel_key.c`), so a passphrase holder can compute
   every epoch. There is no forward secrecy.
5. **Social graph and metadata.** Who talks to whom, when, how often, and how
   much. Cleartext headers carry destination addresses; beacons carry source
   addresses and node names. Largely exposed today (section 4).
6. **Device integrity and availability.** A node that runs attacker firmware
   is worse than a dead node; a dead node still costs the mesh coverage.

## 3. Current protections

Every claim below was verified against the cited file on `main` at the time
of writing.

### Channel payload encryption (AES-256-GCM)

Channel messages (`PKT_TYPE_DATA`) are encrypted with AES-256-GCM using a
16-byte tag and a 12-byte nonce (`components/crypto/crypto_esp.c`,
`components/crypto/include/crypto.h`). Channel keys are derived from the
channel passphrase via SHA-256 then HKDF-SHA256
(`components/channel/channel_key.c`). The mbedtls implementation is
hardware-accelerated on ESP32-S3; the host test build uses OpenSSL
(`components/crypto/crypto_host.c`).

What this protects: payload confidentiality and integrity against anyone
without the channel key.

What it does not hide: the 12-byte cleartext header (version, type, flags,
hop limit, destination address, packet id), the 4-byte cleartext source
address field, ciphertext length, and transmission timing.

### AEAD header binding

The AAD for encrypted DATA packets is the serialized 12-byte header with the
`hop_limit` byte zeroed (`bramble_header_build_aad` in
`components/packet/packet.c`), built identically by the originator
(`send_data_packet` in `main/mesh_task.c`) and the destination
(`handle_data`). A forwarder or injector cannot splice a captured ciphertext
under a modified header (changed destination, flags, or packet id) without
failing the tag check, while relays can decrement `hop_limit` in flight
(`forward_data_packet`) without breaking authentication. The cost of that
exclusion: `hop_limit` itself is unauthenticated, so an injector can rewrite
it on a captured ciphertext; duplicate suppression keyed on the
authenticated `packet_id` bounds what re-injection achieves while the dedup
entry lives.

As of `feat/wire-format-security-batch`, the 4-byte cleartext `src_addr`
field that follows the header is also bound into the AAD
(`bramble_build_aead_aad` in `components/packet/packet.c`: the masked
header plus a 4-byte little-endian `src_addr`, 16 bytes total), not
excluded from it. Both the originator (`send_data_packet`) and the
destination (`handle_data`) pass the same `src_addr`, so tampering it after
origination now fails the GCM tag instead of silently misattributing the
message. An authenticated copy of the source address also still travels
inside the encrypted payload for channel messages
(`components/channel/channel_msg.c`); receivers use that inner copy for
channel traffic, where the outer `src_addr` field is zeroed rather than
meaningful (see the DATA packet format in
`docs/bramble-protocol-spec.md` section 4.4).

### Nonce generation

As of `feat/wire-format-security-batch` (wire v2), nonces for DATA and
LOCATION packets are no longer random. Each node keeps a single node-global
48-bit deterministic counter (`components/nonce_counter`), seeded with the
node's address and a random per-boot salt and persisted to NVS with a
reserve-ahead ceiling: a counter value is never issued unless the ceiling
covering it has already been durably written, so a crash can never reveal a
counter value NVS does not already know about, and a persistent write
failure stops issuing nonces entirely (fail-closed: drop beats reuse). A
single mutex around the one `nonce_counter_next` call site
(`main/mesh_task.c`) serializes the RPC, UI, and mesh-task producers that
can all reach it concurrently. This replaces the previous RNG-per-message
scheme's probabilistic uniqueness argument with a structural one: uniqueness
now holds as long as the persisted ceiling invariant holds, not as a
birthday-bound argument over 96 random bits. The simulator bridge still
builds its nonces with a sim-local helper (`sim_build_nonce` in
`simulator/gosim/bridge.c`: src_addr, counter, 4 random bytes) that predates
and is independent of this counter, so simulated nodes and real nodes
generate nonces differently.

Residual: the counter is visible in the cleartext nonce field on every
wire packet. An observer who cannot decrypt anything can still extract it
(`nonce_counter_extract`) and use it to order and count messages from a
given source address across a boot session, a metadata linkability the
prior random-nonce scheme did not have. See "Residual risks" (section 5).

### Trial decryption across channels, constant-trial loop

Incoming data packets are trial-decrypted against all configured channels
(`components/channel/channel_msg.c`). The loop deliberately tries every
channel even after a match to flatten timing differences between "matched
channel 0" and "matched channel 15". Epoch catch-up advances a channel's key
up to 256 derivations to recover from missed rekeys (`CHANNEL_EPOCH_CATCHUP_MAX`
in `components/channel/include/channel_msg.h`). Catch-up attempts are
rate-limited per channel by a token bucket (capacity one full 256-attempt
recovery, refilled every 10 seconds; constants and the legitimate-recovery
argument in `channel_msg.h`), bounding the CPU an attacker can burn with
undecryptable garbage to about 26 GCM attempts per second per channel.
Successful recoveries are refunded, so only failed catch-up work is
charged and legitimate deep-drift recovery never drains its own bucket;
an attacker sustaining garbage can still hold a bucket near empty, which
delays (not prevents) a concurrent deep rejoin until a quiet refill
window. That trade is accepted: the cap exists to bound CPU.

### RREQ source pseudonymization

Route requests do not carry the originator's address in the source field.
`initiate_discovery` (`main/mesh_task.c`) computes
`HMAC-SHA256(private_key, address || query_id)` truncated to 4 bytes and
sends that as `encrypted_source`. Every attempt, including each retry of the
same discovery, floods under a fresh `query_id` and therefore a fresh
pseudonym, so the pseudonyms themselves are unlinkable to each other as
well as across discoveries. The unlinkability is scoped to the identifier:
the cleartext destination address plus the fixed +5s/+15s retry schedule
still let a passive observer group the attempts of a single discovery by
timing and target. Nothing stores a pseudonym-to-address map; returning RREPs are
correlated by `query_id` against the pending-discovery table, which
remembers every attempt's query_id. One reach trade-off: retries flood with
hop limit 8 instead of the first attempt's 4 (expanding-ring discovery), so
a retried discovery exposes its pseudonymized request to a wider set of
relays and passive observers.

What it does not hide: the *destination* address of the RREQ is cleartext in
the header, request sizes and timing are observable, and on the first hop the
cleartext `prev_hop` field equals the originator's real address while
`hop_count` is 0 (`rreq_build_originator` in
`components/routing/discovery.c`), so any observer who hears the first
transmission identifies the originator anyway. The pseudonym helps only
against observers who hear the RREQ after at least one forward.

### Beacon integrity check, staged toward authentication (SEC-H2)

Beacons carry a 16-byte truncated HMAC-SHA256 computed over the 32 fixed
beacon fields (header, source address, pubkey hash, telemetry, flags, and
network time) plus the optional node name, which is serialized *after*
`auth_hmac` on the wire (`bramble_beacon_serialize` in
`components/packet/packet.c`; `beacon_compute_hmac` in
`components/routing/beacon.c` concatenates the fixed prefix with the
length-prefixed name, skipping over `auth_hmac`'s own bytes, since a
one-shot HMAC call cannot cover two non-contiguous wire regions with a gap
between them any other way). **Red-team panel fix**: the name was
previously excluded entirely, so an attacker could rewrite any captured
beacon's display name and it still verified, spoofing peer names in the
neighbor table/UI even under a provisioned key; the name is now covered.
Verification happens on
receipt (`handle_beacon` in `main/mesh_task.c`) and is now constant-time
(`beacon_verify_hmac`, XOR-accumulate, no early exit). When a network key
is provisioned, the key is a distinct HKDF subkey of it (salt
`"bramble-beacon-v2"`), not the channel PSK; unprovisioned, the key still
derives from the *public, well-known* `BRAMBLE_PUBLIC_CHANNEL_PSK`
(`"bramble-default"` in `components/crypto/include/crypto.h`), logged
explicitly as integrity-only in that case. Unprovisioned, the HMAC still
proves only that the sender runs Bramble-compatible code, not that it is a
trusted member. Provisioned, it proves the sender holds the network key,
which is real authentication against outsiders, but not against another
key holder (section 5) and not against replay of a captured, genuinely
valid beacon (section 5). Do not treat a provisioned beacon HMAC as more
than that.

### RREQ origination rate limiting

A node limits its *own* route discoveries to one per (source, destination)
pair per 30 seconds (`rreq_rate_allow` in `components/security/security.c`,
called from `initiate_discovery` in `main/mesh_task.c`). This bounds
self-inflicted flood, not third-party flood: see section 4 for the missing
forward-path limit.

### Duplicate suppression

Received packets are deduplicated on `packet_id XOR (type << 24)` within a
60-second window (`components/dedup/dedup.c`, key construction in
`mesh_process_rx_packet` in `main/mesh_task.c`). This is loop suppression for
the flooding mesh. It is not replay protection: the fields are
unauthenticated and the window is 60 seconds (section 4).

### RPC authentication, on by default

On first boot the firmware generates a 32-hex-char (128-bit) bearer token
from the hardware RNG and persists it in NVS
(`identity_ensure_ws_auth_token` in `components/identity/identity.c`; the
generation call sites run only after Wi-Fi or BT RF init, when
`esp_random` is fully entropic). The WebSocket upgrade accepts
`Authorization: Bearer <token>`, or `?token=` as the query parameter that
is the only mechanism available to browser WebSocket clients (header auth
is preferred for everything else because URLs leak into logs and
history). The Wi-Fi config POST endpoint is gated by the same token,
accepted as a form field so the AP-mode setup portal can submit it, and
posts that do not prove the token must additionally pass a CSRF check:
an Origin header gets the full origin policy below, a Referer without an
Origin gets a same-origin test, and posts carrying neither header (curl,
scripts) pass through to the token gate, since they are not CSRF-able
(`ws_config_post_allowed` in `main/ws_origin.c`). BLE
requires the token as the first write on a new connection, throttled to one
attempt per 100 ms after failures (`components/ble/ble_server.c`).
Comparison is constant-time and length-independent: a fixed 128-iteration
padded compare shared by both transports (`main/ct_strcmp.h`).

Connections without credentials are accepted but may call only a
two-method identification allowlist, `bramble.ping` and
`bramble.getVersion`, enforced in the dispatcher before method lookup so
the method table cannot be enumerated (`components/rpc/rpc_auth.c`,
`rpc_dispatch_authed` in `components/rpc/rpc_dispatcher.c`). The same
boundary holds on the read side: server-push notifications, which carry
decrypted message content, GPS events, and peer locations, are delivered
only to authenticated connections on both transports
(`rpc_auth_notify_filter` feeding `ws_notify_cb` in `main/ws_server.c`;
the gated dispatcher transport in `components/ble/ble_server.c`). Wrong
credentials close the connection. User-supplied tokens shorter than 16
bytes are rejected (`rpc_set_auth_token` in `main/rpc_methods.c`). If the
token cannot be read or persisted (NVS failure), the device fails closed:
full access is unreachable rather than open.

The serial CLI dispatches RPC without authentication *by design*: physical
USB access is the pairing bootstrap. `bramble pair` reads the token over
serial, and the token is printed to the serial boot log for the same
reason. This is the device-as-secret posture (section 5): whoever holds
the hardware owns it, and today plaintext flash (section 4) makes any
stronger serial story moot.

Auth can be explicitly disabled by an authenticated
`bramble.setAuthToken` call with an empty token; the opt-out persists in
NVS and is logged loudly at boot. The default posture is closed.

### WebSocket Origin allowlist

When a WS upgrade carries an `Origin` header (every browser sends one)
and does not present the valid bearer token, the device allows
same-origin requests (Origin host equals Host header host, any port or
scheme, so the device's IP and mDNS names work), plus origins on an
NVS-stored allowlist managed via the authenticated
`bramble.setAllowedOrigins` / `bramble.getAllowedOrigins` RPCs. Everything
else, including the literal `null` origin, is rejected at upgrade time
with a 1008 close frame (decision logic in `main/ws_origin.c`, enforcement
in `main/ws_server.c`). Requests without an Origin header pass: a client
that is not a browser is not subject to cross-site WebSocket hijacking.

A request that presents the valid token bypasses the Origin check. The
token is the stronger credential and is unreadable cross-site, so a
token-bearing cross-origin page is the owner's webapp (hosted or local
dev), not an attack; gating it would force origin enrollment over serial
before the webapp could ever connect. On a device whose owner explicitly
disabled auth, no connection carries a token, so the Origin check applies
to every WS upgrade and, via the CSRF rule above, to every browser-borne
config POST; those checks are the remaining CSWSH and CSRF defenses on an
opted-out device.

### Wi-Fi setup AP

The fallback configuration AP is WPA2-PSK, not open
(`components/wifi/wifi_manager.c`). The default password is `bramble123`,
compiled in via Kconfig (`components/wifi/Kconfig`), so it gates against
drive-by association only, not against anyone who has read the source.

### OTA image signing and transport security

Every firmware image is signed at build time with an RSA-3072 key
(Secure Boot V2 signature block appended to `bramble.bin`;
`CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` in `sdkconfig.defaults`). On OTA,
the device verifies the incoming image's signature against the public key
embedded in the running app's own signature block and fails closed
(`esp_https_ota_finish` / `esp_ota_end`); this holds on the dev-only HTTP
path too. Release images are signed in CI with a key held only as a CI
secret; the matching public key is committed at `keys/ota-release-pub.pem`
and CI verifies every built artifact against it. See
`docs/design/ota-signing.md` for the trust model and rotation.

Transport: `https://` OTA URLs are fetched with certificate validation
against the ESP x509 bundle, with hostname check enabled
(`components/ota/ota.c`). Plain-`http://` OTA is compiled out unless
`CONFIG_BRAMBLE_OTA_ALLOW_HTTP` is set. The device only fetches from its
allowlisted OTA origin (default `https://bramblemesh.org/ota/`), changeable
solely through the authenticated `bramble.otaSetOrigin` RPC.

### Identity generation

X25519 keypairs are generated from the hardware RNG with correct clamping,
and scalar multiplication uses mbedtls with the RNG callback supplied for
side-channel blinding (`crypto_generate_identity` in
`components/crypto/crypto_esp.c`). Address collisions (two nodes hashing to
the same 4-byte address with different pubkeys) are detected from beacons and
resolved by regenerating the local identity (`identity_check_collision` in
`components/identity/identity.c`, handled in `main/mesh_task.c`).

### Sybil heuristic (log-only)

An RSSI-clustering check flags groups of "neighbors" arriving at suspiciously
similar signal strength (`sybil_check_rssi_cluster` in
`components/security/security.c`). It only logs; it drops nothing and feeds
no decision.

### Mailbox content

Mailbox nodes store the raw forwarded packet, which for channel messages is
ciphertext; the mailbox never holds plaintext it could not already read
(`forward_data_packet` to `mesh_mailbox_store` in `main/mesh_task.c`,
`components/mailbox/mailbox.c`). Entries are RAM-only, capped per
destination, and expire.

### Direct message end-to-end encryption (SEC-C2, closed)

Unicast DMs no longer ride channel keys. `mesh_send_message`
(`main/mesh_task.c`) never transmits a DM under a channel key: a per-peer
session is established with an authenticated X25519 handshake carried
inside `PKT_TYPE_DATA` envelopes (`app_type = APP_TYPE_KE`), using a
role-symmetric quad-DH key schedule (`components/dm_session`) that rejects
low-order X25519 points and produces a 7-digit SAS for out-of-band
verification. If no session exists yet, the send queues and triggers a
handshake rather than falling back to a channel key; on handshake timeout
the caller sees `onAck failed reason="no_secure_session"`. The retired
`PKT_TYPE_KEY_EXCHANGE` (0x06) standalone packet is gone; see
`docs/bramble-protocol-spec.md` section 4.25. This closes the two gaps
this document previously listed under "Direct messages are encrypted with
shared channel keys" and "DMs are encrypted under the well-known public
PSK". Residual: no out-of-band SAS comparison UX ships yet (section 5), and
a compromised or malicious mesh member is still an insider by definition,
same as any symmetric-key system: authenticating a DM to a specific peer
does not defend against that peer itself misbehaving.

**Session-table exhaustion DoS, closed (red-team panel fix).** A
first-contact INIT needs no secret (a self-generated keypair passes
`dm_verify_init`'s address-binding check trivially), and previously landed
straight in `DM_STATE_ACTIVE`/`verified=0` without ever touching
`DM_STATE_HANDSHAKING` or its cap. Because `dm_alloc` (`components/dm_session`)
protected every `DM_STATE_ACTIVE` slot from eviction regardless of
`verified`, an attacker sending `DM_MAX_SESSIONS` forged first-contact
INITs filled the table with permanently-unevictable sessions, killing all
future DM establishment (with anyone) until reboot. `dm_alloc` now
protects only VERIFIED `ACTIVE` sessions from eviction; an UNVERIFIED
`ACTIVE` session is evictable, LRU-ordered by a `last_active_ms` timestamp
that every real send/receive through the session bumps (`main/mesh_task.c`),
so a genuinely-active first-contact conversation still outlives an idle
forged flood.

### Location payload encryption (SEC-C1, closed)

LOCATION packets are AES-256-GCM encrypted end to end, on both the
channel-shared and direct-session paths (`mesh_send_location_packet`,
`handle_location` in `main/mesh_task.c`). The sharing tier travels inside
the authenticated plaintext (byte 0) rather than the cleartext header, and
every LOCATION ciphertext pads to one canonical size regardless of tier, so
ciphertext length does not leak which tier was chosen. A per-sender replay
window (shared with DATA, below) rejects replayed location updates; a
below-window location update is dropped, never deferred, since location is
real-time and a stale position accepted late is worse than one dropped.
This closes the gap this document previously listed under "Location
packets are transmitted entirely in cleartext". Residual: an observer still
learns that a node is running location sharing and roughly how often it
updates, from packet type and timing alone, even with coordinates hidden
(section 5).

### Per-sender replay windows for DATA and LOCATION (SEC-M1, closed)

Received DATA and LOCATION packets are checked against a per-sender sliding
replay window keyed on `(src_addr, nonce_counter_extract(nonce))`, enforced
post-decrypt on the authenticated counter, not the cleartext header
(`components/replay_window`). A tier-1 (immediate) window covers the common
case; a tier-2 path defers acceptance of a below-window counter until the
authenticated `sent_at` timestamp inside the GCM plaintext can be corroborated
against synchronized network time, and fails closed (rejects) if the two
endpoints are not time-synced. This replaces the old unauthenticated
`packet_id`-keyed dedup as the replay defense for these two packet types
(dedup remains, and remains loop-suppression only, for packet types that are
not authenticated per-message: routing control, beacon, ACK, and delivery
receipt).

**Public-channel exclusion, closed (red-team panel fix).** The window is
only fed a decrypt's src_addr when that src_addr is trustworthy
(`channel_source_is_replay_trustworthy` in `components/channel`): a
session or secret-channel src_addr costs a session key or channel
membership, but `BRAMBLE_PUBLIC_CHANNEL_PSK` is known to literally
everyone, so a public-channel decrypt's src_addr was previously a
free-to-forge claim fed straight into this SHARED window. An attacker
could encrypt a packet under the public key claiming `src_addr=victim`
and slam the victim's high-water mark, causing the victim's own later,
genuine packets to read `BELOW_WINDOW` and drop: a mesh-wide DoS on
location and chat delivery reachable by anyone, not just channel members.
Public-channel traffic is now excluded from this window entirely and
relies on the pre-existing `packet_id`/type dedup for loop suppression
instead; it has no replay protection of its own (residual, section 5).

**Tier-1/tier-2 dedup gap, closed (red-team panel fix).** A CHAT message
accepted via tier-1 is now also recorded in the tier-2 deferred cache
(`replay_deferred_mark_seen`). Previously it was not: a counter accepted
in-window and later aged out of the 64-entry tier-1 window was in NEITHER
dedup structure, so a captured, genuinely-delivered chat packet replayed
successfully once the sender's window moved past it (tier-1 read
`BELOW_WINDOW`, and a tier-2 cache that had never heard of the original
acceptance treated the replay as a fresh, legitimate deferred delivery).

Residuals: a 64-entry sender table and a 128-entry tier-2 LRU evict the
oldest tracked sender under load, which loses replay history for an
evicted-then-later-returning sender (needs 64, respectively 128,
concurrent distinct senders to matter).

### Control-plane authentication: RREP, RERR, ACK, delivery receipt, beacon (SEC-H1, SEC-H2, NEW-SEC-4, NEW-SEC-8; mitigation staged, not closed)

Every routing and reliability control message that previously carried no
authentication now carries a network-key HMAC, verified before the message
is acted on: RREP (`rrep_verify` in `components/routing/discovery.c`,
before `route_install`), RERR (`rerr_verify` in
`components/routing_auth`, before any route teardown), ACK and delivery
receipt (`ack_verify`/`receipt_verify` in `components/routing_auth`, before
retransmission is cancelled, a message is marked delivered, or the packet
is forwarded), and beacon (`beacon_verify_hmac` in
`components/routing/beacon.c`, now constant-time). Each MAC's covered field
set was chosen by reading the corresponding forwarding function first and
excluding exactly the fields it legitimately mutates in flight (`next_hop`,
`header.dest_addr` on RREP; `reporter_addr`, `packet_id` on RERR;
`relay_path`, `hop_count`, `header.hop_limit` on ACK and delivery receipt),
so a legitimate forward never breaks verification and a relay-mutated field
is never trusted as authenticated.

**This does not close SEC-H1, SEC-H2, NEW-SEC-4, or NEW-SEC-8.** The key
behind every one of these MACs comes from `components/network_key`, which
ships unprovisioned by default: `network_key_get()` falls back to a key
derived from `BRAMBLE_PUBLIC_CHANNEL_PSK`, a public, compile-time constant
(`"bramble-default"`). Anyone who reads that constant, which is checked
into this repository, derives the identical fallback key and forges a
valid MAC for any of these five message types. A real per-fleet key can be
loaded via the authenticated `bramble.setNetworkKey` RPC (gated the same
way as `bramble.setAuthToken`) or from NVS at boot, but this batch does not
ship key generation, distribution, or rotation UX. Closure of these four
findings requires provisioning **plus** the additional work in section 5
below (per-message freshness and per-node beacon identity); provisioning
alone is not sufficient and must not be described as closing them.

## 4. Known gaps in the current implementation

Facts of the code on `main` today. Each entry shrinks or disappears in the
same PR that fixes it.

- **RREP, RERR, ACK, delivery receipt, and beacon authentication is staged,
  not closed** (SEC-H1, SEC-H2, NEW-SEC-4, NEW-SEC-8): the verify logic
  described in section 3 is real, but every key it checks against is
  forgeable by anyone who reads the public `BRAMBLE_PUBLIC_CHANNEL_PSK`
  fallback until a real network key is provisioned, and even a provisioned
  key does not stop replay of a captured, genuinely-valid message on any of
  these five types (section 5). Treat all five as unauthenticated against a
  knowledgeable insider until both provisioning and the section 5 follow-up
  work land.
- **RREQ forwarding is not rate-limited**; the 30-second limiter applies only
  to locally-originated discoveries, so a flood of foreign RREQs is forwarded
  without restriction (`handle_rreq` in `main/mesh_task.c`).
- **Replay protection does not cover routing or reliability control
  traffic.** DATA and LOCATION now have real per-sender replay windows
  (section 3), but RREP, RERR, beacon, ACK, and delivery receipt still
  dedup only on unauthenticated `packet_id` and type inside a 60-second
  window (`components/dedup/dedup.c`), which is loop suppression, not
  replay protection: a captured, genuinely-valid message on any of these
  five types replays until the dedup window expires (section 5).
- **The identity private key, all channel keys, the RPC auth token, and,
  once provisioned, the network key are stored as plaintext NVS entries,
  and message history is plaintext SPIFFS**, with flash encryption, NVS
  encryption, and secure boot all disabled in the build
  (`components/identity/identity.c`, `components/channel/channel_storage.c`,
  `components/msg_store/msg_store_spiffs.c`, `components/network_key`;
  `sdkconfig` has `CONFIG_SECURE_FLASH_ENC_ENABLED`, `CONFIG_NVS_ENCRYPTION`,
  and `CONFIG_SECURE_BOOT` all unset).
- **The WebSocket transport is plaintext HTTP on port 80**, so an on-path
  LAN attacker reads all RPC traffic including the bearer token; the
  `?token=` query parameter that browser clients must use additionally
  leaks via URL logs and browser history (`main/ws_server.c`).
- **OTA signatures are enforced at update time, not at boot.** Images are
  signed and verified on every OTA write, the update origin is allowlisted,
  and a soft anti-rollback floor rejects downgrades, but without burned
  eFuses the bootloader will still boot whatever sits in flash. An attacker
  with physical flash access (USB, JTAG) can write unsigned firmware, and
  can erase the NVS rollback floor. Hardware Secure Boot V2 plus eFuse
  anti-rollback closes this; it is staged behind bench validation on a
  sacrificial board (human-gated). Consistent with the device-as-secret
  posture above, physical possession is already treated as compromise.
- **Mailbox flush trust follows the same staged beacon key as everything
  else in section 3's control-plane entry**: unprovisioned, the beacon key
  is still public-PSK-derived, so a forged beacon for a victim address makes
  a mailbox transmit that victim's queued ciphertext (`handle_beacon`
  mailbox flush in `main/mesh_task.c`); provisioned, forgery requires the
  network key but a captured valid beacon still replays (section 5).
- **Beacons broadcast node name, battery percentage, uptime, neighbor count,
  and mailbox capability in cleartext** (`mesh_send_beacon` in
  `main/mesh_task.c`).

## 5. Residual risks that remain by design

These do not go away when section 4 empties out.

- **Channel insiders read channel traffic.** A channel key is a membership
  credential; everyone holding it reads everything on that channel. That is
  what a group chat is. Compartmentalization across channels is the control,
  not cryptography within one.
- **A key-holding insider can lie in routing.** Metric inflation, selective
  forwarding, and silent dropping by a legitimate mesh member are detectable
  at best, not preventable. Authentication binds messages to members; it
  cannot make members honest.
- **Mailbox and relay operators see ciphertext, sizes, and timing.** A
  store-and-forward node necessarily learns that traffic for address X
  exists, how big it is, and when it arrived.
- **RF presence is detectable and locatable.** Every transmission announces
  that a node exists, and direction finding works on any transmitter.
  Bramble can hide what you say and largely who you say it to, never that a
  radio transmitted.
- **Serial RPC is unauthenticated.** Physical USB access is the pairing
  bootstrap: `bramble pair` retrieves the auth token over serial, and the
  serial console dispatches RPC at full privilege (`main/cli.c`). This is
  deliberate device-as-secret design, not an oversight: a stolen device
  already yields its plaintext flash (section 4), and a forgotten token
  must not brick the owner out of their own hardware.
- **Cleartext routing headers.** Destination addresses must be readable by
  relays for multi-hop forwarding to work at this power and duty-cycle
  budget. Onion routing over LoRa airtime budgets is not a trade Bramble
  makes; an in-range observer learns destination addresses of forwarded
  unicast traffic.
- **Traffic analysis from sizes and timing.** Padding and cover traffic cost
  airtime, which is the scarcest resource in the system. Bramble spends
  airtime on messages, not chaff (a small dummy-traffic component exists in
  `components/security/dummy_traffic.c`, but nothing schedules it on the
  air today).
- **Insider forgery of control-plane messages is inherent to a shared
  symmetric network key.** Every RREP, RERR, ACK, delivery receipt, and
  beacon MAC in section 3 proves "signed by a network-key holder", not
  "signed by a specific node". Once the network key is provisioned, any
  holder can still forge a control message on behalf of any other holder;
  the design goal of provisioning is excluding non-members, not
  distinguishing among members. This is the same shape as "a key-holding
  insider can lie in routing" above, applied to the newly-authenticated
  message types.
- **Replay of RREP, RERR, beacon, ACK, and delivery receipt is not closed by
  network-key provisioning.** None of the five section 3 MACs bind a nonce,
  sequence number, or timestamp; provisioning stops a non-member from
  forging a fresh message but does nothing to stop anyone from recording
  and re-transmitting a captured, genuinely-valid one. Ranked by impact:
  **RERR replay is worst**: `route_lookup`/teardown in `handle_rerr`
  (`main/mesh_task.c`) is ungated by any freshness check, so a captured
  valid RERR re-tears-down a live route on replay, a persistent, repeatable,
  targeted denial of service, partially self-limiting only because a
  torn-down route gets rediscovered. **RREP replay** resurrects a stale
  route: `route_install` (`main/mesh_task.c`) is ungated by discovery
  freshness, so a captured valid RREP re-installs an old path. Beacon, ACK,
  and delivery-receipt replay have narrower blast radii (a stale mailbox
  flush trigger; a spuriously re-cancelled retry or re-marked delivery; a
  redundant forwarded receipt) but are equally unauthenticated for
  freshness. Closing any of this requires per-message freshness or sequence
  binding added to each of the five MACs, a distinct piece of work from
  provisioning itself.
- **The timesync bootstrap quorum can still be won by one key holder with
  multiple addresses (NEW-SEC-4).** The beacon HMAC gate and the
  bootstrap-offset clamp (section 3) both require holding the network key,
  but neither requires holding a *distinct* identity per beacon. A single
  insider who holds the network key and transmits under three fabricated
  source addresses satisfies `CORROBORATION_REQUIRED` (3 distinct sources)
  entirely on their own and commits an arbitrary first time offset; the
  bootstrap clamp only bounds a proposal against the *existing* pending
  pool, which that same insider fully controls when they are the only
  contributor. Compounding this, `timesync_is_confident` never reverts to
  false once `synchronized` becomes true (`components/timesync/timesync.c`),
  so a bad offset committed at bootstrap is trusted indefinitely, not just
  until the next corroboration round. Closing NEW-SEC-4 needs provisioning
  **plus** a per-node beacon identity binding (so one network key cannot
  mint an arbitrary number of distinct-looking sources), on top of the
  general control-plane replay work above.
- **RREP `next_hop` poisoning is inherent, not a bug this batch can close.**
  `next_hop` is necessarily a relay-mutated, unauthenticated field: each
  forwarder must be able to rewrite it to route the reply back toward the
  originator, so it can never be included in `auth_hmac`'s covered set
  without breaking every legitimate forward. A malicious relay can always
  redirect a RREP's next hop within what its position in the network
  already lets it do; this is a property of hop-by-hop mutable routing
  fields in general, not something a control-plane MAC can fix.
- **Beacon re-keying lags a runtime `setNetworkKey` call.** RREP, RERR, ACK,
  and delivery-receipt MACs read `network_key_get()` live on every call, so
  a runtime-provisioned key protects them immediately. The beacon key is
  derived once at init and cached (`s_beacon_key` in `main/mesh_task.c`), so
  it keeps using the old (possibly still-unprovisioned) key until the node
  reboots. An operator who calls `setNetworkKey` at runtime does not get
  uniform, instant protection across all five MAC types.
- **DM handshake SAS verification has no UX.** `dm_derive_sas` produces a
  7-digit short authentication string, but nothing in this batch surfaces
  it for an out-of-band comparison. A MitM during first-contact handshake
  is only detectable if the two users compare the code through some channel
  outside Bramble itself; today, nothing prompts them to.
- **The nonce counter is metadata, not just a cryptographic nonce.**
  Because DATA/LOCATION nonces are now a deterministic counter rather than
  random bits (section 3), an observer who cannot decrypt anything can
  still read the counter off the wire and use it to order and roughly
  count a given source address's messages across a boot session. The
  previous random-nonce scheme did not expose this.
- **Public-channel traffic has no replay protection of its own.** Excluding
  a public-channel decrypt's forgeable src_addr from the shared replay
  window (section 3, red-team panel fix) closes the shared-window
  poisoning DoS, but leaves public-channel DATA/LOCATION relying solely on
  the pre-existing `packet_id`/type dedup (60-second window, loop
  suppression only). A captured public-channel packet can be replayed
  within that 60-second window; this is accepted, since public-channel
  content has no confidentiality expectation in the first place (the key
  is public) and the alternative (a shared window keyed on a forgeable
  identity) was actively worse.
- **A first-contact DM session can, in principle, be evicted under a
  sustained attack if it goes idle.** `dm_alloc`'s eviction (section 3,
  red-team panel fix) protects a genuinely-active UNVERIFIED session via
  its `last_active_ms` timestamp, but a first-contact session that is
  established and then never sends or receives again before the SAS check
  completes is, like any idle UNVERIFIED session, eligible for eviction
  under table pressure. This trades a permanent, total DM outage (the
  pre-fix behavior) for a narrow, activity-gated one; an idle session that
  is evicted must simply re-handshake.
- **A 64-sender or 128-entry LRU table can be evicted under enough
  concurrent senders.** The DATA/LOCATION tier-1 replay table (64 senders)
  and tier-2 deferred table (128 entries) evict the least-recently-seen
  entry under load. An evicted sender who later returns starts with no
  replay history, which only matters once a mesh sustains that many
  concurrent distinct senders.
- **Minor, non-exploitable robustness notes:** `network_key_mac`'s
  `label || data` concatenation is not length-prefixed, which would be
  ambiguous for attacker-chosen labels, but every label is a fixed,
  prefix-free internal constant (`"bramble-rrep-v2"`, `"bramble-rerr-v2"`,
  `"bramble-ack-v2"`, `"bramble-receipt-v2"`, `"bramble-beacon-v2"`), so
  this is not reachable today. ACK and delivery-receipt `relay_path` and
  `hop_count` remain intentionally unauthenticated (they are per-hop
  telemetry, not security-relevant), so a replayed ACK or receipt can
  still trigger a bounded re-forward and a cosmetic hop-trail change in the
  UI even where the core `src_addr`/packet-id binding holds. The LOCATION
  channel-message decode path does not assert `app_type == APP_TYPE_LOCATION`
  before parsing (defense-in-depth only; it is inside the AEAD trust
  boundary and memory-safe either way).

## 6. How to think about Bramble's privacy

Plain words, current state:

- **What you say on a private channel is protected** from outsiders by
  strong, standard encryption, as long as the passphrase is good and was
  shared securely. Everyone *on* the channel reads everything on it.
- **Location sharing is now encrypted end to end.** Coordinates and the
  sharing tier travel under AES-256-GCM; an outsider without your keys
  cannot read them. They can still tell that you are sharing location and
  roughly how often, from packet type and timing alone.
- **Direct messages are now end-to-end encrypted per conversation**, not
  shared with everyone on a channel. A DM is protected by a session key
  negotiated directly with the recipient; other channel members cannot read
  it. There is no out-of-band way yet to confirm you are talking to the
  right person on first contact (a short verification code exists in the
  code but nothing in the app surfaces it to compare with the other side).
- **Who you talk to is more exposed than what you say.** Destination
  addresses, beacons with your node's name, and message timing are visible
  to anyone in range.
- **Anyone nearby can tell your radio exists** and a motivated person can
  find it. If your safety depends on nobody knowing you transmit at all,
  no LoRa mesh is the right tool.
- **Treat the physical device as the secret.** With flash unencrypted,
  anyone who takes your node can extract your keys and message history with
  free tools in minutes. Losing the device means rotating every channel it
  was on.
- **Put the node on a Wi-Fi network you trust.** The control interface now
  requires a per-device token by default (get it with `bramble pair` over
  USB), but the connection itself is unencrypted HTTP: someone recording
  traffic on your Wi-Fi can capture the token and your messages.

The honest one-line summary of today's build: Bramble encrypts channel
message content well, and most of its other privacy properties do not hold
in the current code. This document must keep matching the code.
