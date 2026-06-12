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
header is bound to the ciphertext as AEAD associated data. Routing control
traffic, location packets, mailbox triggers, acknowledgements, and delivery
receipts are forgeable (section 4).
Jamming is not defendable at this layer and is accepted (section 5).

**Malicious mesh member.** A node that legitimately holds one or more channel
keys: an invited member gone bad, or a stolen key. They decrypt everything on
those channels, can impersonate any source address inside those channels, and
participate fully in routing. The design goal is to limit them to the channels
they hold keys for and to make routing lies detectable; today routing lies are
not detectable (section 4).

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
  `main/ws_server.c`) defaults to open access with no token configured. The
  transport is plaintext HTTP. Anyone on the LAN can read messages, send
  messages, rotate keys, and trigger OTA. Defense is opt-in today (section 4).
- *Malicious web page in a browser on the LAN.* The WebSocket upgrade path
  performs no `Origin` check, so any web page a LAN user visits can script
  connections to the node's WS endpoint (section 4).
- *BLE proximity.* The BLE RPC transport gates on the same token as WS, with a
  first-write handshake and throttled retries (`components/ble/ble_server.c`),
  but the token defaults to unset, which means open access (section 4).

**Compromised OTA source.** An attacker who controls the firmware download
server, the URL given to the device, or the TLS path. HTTPS with certificate
bundle validation is enforced for `https://` URLs (`components/ota/ota.c`),
but images are not signed, there is no secure boot, no anti-rollback, and the
URL is supplied by the RPC caller. An attacker with RPC access (default-open,
see above) installs arbitrary firmware (section 4).

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

The 4-byte cleartext `src_addr` field that follows the header is *not*
in the AAD, but an authenticated copy of the source address travels inside
the encrypted payload (`components/channel/channel_msg.c`), and receivers use
the inner copy. The outer field is unauthenticated bytes on the wire.

### Nonce generation

Nonces are 96 random bits per message from the ESP32 hardware RNG
(`channel_msg_encrypt` in `components/channel/channel_msg.c` calls
`crypto_random`). There is no counter discipline and no persistence across
reboot; uniqueness rests entirely on RNG quality and the birthday bound. At
LoRa data rates the collision probability is small but this is a
probabilistic argument, not a structural one. A deterministic nonce helper
(`crypto_build_nonce` in `components/crypto/crypto_esp.c`) exists but has no
callers in the firmware; the host simulator bridge does use it
(`simulator/gosim/bridge.c`), so simulated nodes and real nodes generate
nonces differently.

### Trial decryption across channels, constant-trial loop

Incoming data packets are trial-decrypted against all configured channels
(`components/channel/channel_msg.c`). The loop deliberately tries every
channel even after a match to flatten timing differences between "matched
channel 0" and "matched channel 15". Epoch catch-up advances a channel's key
up to 256 derivations to recover from missed rekeys (`CHANNEL_EPOCH_CATCHUP_MAX`
in `components/channel/include/channel_msg.h`); see section 4 for the CPU
cost this hands an attacker.

### RREQ source pseudonymization

Route requests do not carry the originator's address in the source field.
`initiate_discovery` (`main/mesh_task.c`) computes
`HMAC-SHA256(private_key, address || query_id)` truncated to 4 bytes and
sends that as `encrypted_source`; a fresh `query_id` per request makes
pseudonyms unlinkable across requests. The mapping is held locally so
returning RREPs can be correlated (`pseudonym_store`, expiring after 60
seconds).

What it does not hide: the *destination* address of the RREQ is cleartext in
the header, request sizes and timing are observable, and on the first hop the
cleartext `prev_hop` field equals the originator's real address while
`hop_count` is 0 (`rreq_build_originator` in
`components/routing/discovery.c`), so any observer who hears the first
transmission identifies the originator anyway. The pseudonym helps only
against observers who hear the RREQ after at least one forward.

### Beacon integrity check (not authentication)

Beacons carry a 16-byte truncated HMAC-SHA256 computed over the 32 fixed
beacon fields only: header, source address, pubkey hash, telemetry, flags,
and network time (`beacon_compute_hmac` in `components/routing/beacon.c`
MACs the first `BEACON_SIZE - 16` bytes). The optional node name is
serialized *after* the HMAC field (`bramble_beacon_serialize` in
`components/packet/packet.c`) and is not covered by it, so the name is
unauthenticated even in the integrity sense. Verification happens on receipt
(`handle_beacon` in `main/mesh_task.c`). The key is derived from the
*public, well-known* channel PSK (`BRAMBLE_PUBLIC_CHANNEL_PSK` =
"bramble-default" in `components/crypto/include/crypto.h`; derivation at
mesh init in `main/mesh_task.c`). Because anyone can derive this key, the
HMAC proves only that the sender runs Bramble-compatible code; it rejects
corruption and non-Bramble traffic, not attackers. It is an integrity check,
not authentication, and nothing in the code treats it as more than that.

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

### RPC auth token, when configured

If an auth token is set, the WebSocket upgrade requires
`Authorization: Bearer <token>` (or a deprecated `?token=` query parameter),
compared in constant time over a fixed-length loop
(`token_matches_constant_time` in `main/ws_server.c`). The Wi-Fi config POST
endpoint is gated by the same check. BLE requires the token as the first
write on a new connection, throttled to one attempt per 100 ms after failures
(`components/ble/ble_server.c`). The token is *unset by default*; see
section 4.

### Wi-Fi setup AP

The fallback configuration AP is WPA2-PSK, not open
(`components/wifi/wifi_manager.c`). The default password is `bramble123`,
compiled in via Kconfig (`components/wifi/Kconfig`), so it gates against
drive-by association only, not against anyone who has read the source.

### OTA transport security

`https://` OTA URLs are fetched with certificate validation against the ESP
x509 bundle, with hostname check enabled (`components/ota/ota.c`).
Plain-`http://` OTA is compiled out unless `CONFIG_BRAMBLE_OTA_ALLOW_HTTP` is
set. This authenticates the *server*, not the *image*: see section 4.

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

## 4. Known gaps in the current implementation

Facts of the code on `main` today. Each entry shrinks or disappears in the
same PR that fixes it.

- **Location packets are transmitted entirely in cleartext**, including
  coordinates, with the sharing tier readable in the cleartext header flags
  (`mesh_send_location_packet` and `handle_location` in `main/mesh_task.c`;
  serialization in `components/location/location.c`).
- **Direct messages are encrypted with shared channel keys, not end-to-end
  pairwise keys**, so every holder of the channel key reads every DM on it
  (`mesh_send_message` routes through `mesh_send_channel` in
  `main/mesh_task.c`).
- **On a device whose default channel is still channel 0, DMs are encrypted
  under the well-known public PSK** ("bramble-default") and are readable by
  anyone, since `s_default_channel_idx` initializes to 0 and
  `mesh_send_message` uses it (`main/mesh_task.c`).
- **`PKT_TYPE_KEY_EXCHANGE` is defined on the wire but never sent and never
  handled**; it falls through to the unhandled-type branch of
  `mesh_process_rx_packet` (`components/packet/include/packet.h`,
  `main/mesh_task.c`).
- **The RREP `auth_hmac` field is dead**: zeroed at build, never computed,
  never verified, so any node can forge route replies and black-hole traffic
  (`rrep_build_destination` in `components/routing/discovery.c`,
  `handle_rrep` in `main/mesh_task.c`).
- **RERR packets carry no authentication**, so any in-range transmitter can
  mark arbitrary routes broken and fast-fail pending messages (`handle_rerr`
  in `main/mesh_task.c`).
- **ACKs and delivery receipts are forgeable**: `packet_id` is cleartext in
  the DATA header, and `handle_ack` and `handle_delivery_receipt` in
  `main/mesh_task.c` accept unauthenticated packets, so a forged ACK both
  cancels retransmission (`pending_ack_remove`) and marks the message
  `DELIVERED` in the store and UI, letting an in-range transmitter fake
  delivery confirmation while suppressing the retry and mailbox fallback.
- **RREQ forwarding is not rate-limited**; the 30-second limiter applies only
  to locally-originated discoveries, so a flood of foreign RREQs is forwarded
  without restriction (`handle_rreq` in `main/mesh_task.c`).
- **There is no replay protection**: dedup keys on unauthenticated
  `packet_id` and type inside a 60-second window, and the `anti_replay`
  module exists with zero callers (`components/dedup/dedup.c`,
  `components/timesync/anti_replay.c`).
- **The identity private key, all channel keys, and the RPC auth token are
  stored as plaintext NVS entries, and message history is plaintext SPIFFS**,
  with flash encryption, NVS encryption, and secure boot all disabled in the
  build (`components/identity/identity.c`,
  `components/channel/channel_storage.c`,
  `components/msg_store/msg_store_spiffs.c`; `sdkconfig` has
  `CONFIG_SECURE_FLASH_ENC_ENABLED`, `CONFIG_NVS_ENCRYPTION`, and
  `CONFIG_SECURE_BOOT` all unset).
- **RPC defaults to open access on Wi-Fi and BLE**: no token exists until a
  user sets one (`identity_ensure_ws_auth_token` in
  `components/identity/identity.c`), and `bramble.getAuthToken` returns the
  token in cleartext to any connected client (`main/rpc_methods.c`).
- **The WebSocket endpoint performs no `Origin` check and runs plaintext
  HTTP on port 80**, so LAN web pages can script connections and on-path LAN
  attackers read everything including the bearer token
  (`main/ws_server.c`).
- **OTA installs unsigned images from an RPC-supplied URL with no
  anti-rollback**, so RPC access (open by default, above) is firmware-flash
  access (`components/ota/ota.c`, `ota_task` in `main/rpc_methods.c`).
- **Mailbox flush is triggered by an effectively unauthenticated beacon**,
  since the beacon HMAC key is derived from the public PSK; a forged beacon
  for a victim address makes a mailbox transmit that victim's queued
  ciphertext (`handle_beacon` mailbox flush in `main/mesh_task.c`).
- **Epoch catch-up burns up to 256 HKDF-plus-GCM attempts per non-matching
  channel on every received data packet**, legitimate traffic included (a
  packet for one channel triggers the full catch-up loop on every other
  configured channel), an unauthenticated CPU amplification lever
  (`channel_msg_decrypt` in `components/channel/channel_msg.c`).
- **Beacons broadcast node name, battery percentage, uptime, neighbor count,
  and mailbox capability in cleartext** (`mesh_send_beacon` in
  `main/mesh_task.c`).
- **`ct_strcmp` leaks operand length through its loop bound** (`main/ct_strcmp.h`),
  used on the BLE auth path; content comparison is constant-time, length is
  not.
- **The serial CLI is unauthenticated** (`main/cli.c`); anyone with USB
  access has full RPC capability. With flash unencrypted this is currently
  redundant with the line above it, but it remains true independently.

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

## 6. How to think about Bramble's privacy

Plain words, current state:

- **What you say on a private channel is protected** from outsiders by
  strong, standard encryption, as long as the passphrase is good and was
  shared securely. Everyone *on* the channel reads everything on it.
- **Today, do not use Bramble location sharing for anything sensitive.**
  Location packets currently go over the air unencrypted; anyone in radio
  range with cheap hardware can read your coordinates.
- **Direct messages are currently group-readable.** A DM is hidden from
  outsiders, but every member of the channel it rides on can decrypt it. If
  you have not set up a private channel, a DM is readable by anyone at all.
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
- **Put the node on a Wi-Fi network you trust, and set an auth token.** The
  control interface ships open; anyone on the same Wi-Fi can read and send
  your messages and reflash the device until you set a token (and the token
  then travels in plaintext on the LAN).

The honest one-line summary of today's build: Bramble encrypts channel
message content well, and most of its other privacy properties do not hold
in the current code. This document must keep matching the code.
