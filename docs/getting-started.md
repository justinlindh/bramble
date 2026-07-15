# Getting Started: Your First Node

This is the zero-to-first-message path for a single Bramble node: flash it,
connect the web client, understand why a fresh node is silent, provision a
network key, optionally enroll it under a trust anchor, and send a message.
It ties together the reference docs rather than duplicating them; each step
links the doc that owns the detail.

If you are standing up more than one node, do steps 1 and 2 on each, then
provision them all onto the same network key (and, if you want it, the same
trust anchor) in steps 4 and 5.

## 1. Flash the firmware

Two ways to get firmware onto a device:

- **Browser flashing (no toolchain).** The
  [web flasher](https://bramblemesh.org/web-flasher/) flashes a device over
  USB straight from the browser. In its Device Setup step you can expand the
  Security section to set your own RPC auth token or have the device generate
  one on first boot (see [auth.md](auth.md)).
- **From source.** Build and flash with the board-aware scripts documented in
  [BUILDING.md](BUILDING.md). The short version:

  ```bash
  cd ~/src/bramble
  bash scripts/flash.sh local heltec-v3 build
  bash scripts/flash.sh local heltec-v3 flash /dev/ttyUSB0
  ```

  Use `tdeck-plus` instead of `heltec-v3` for a T-Deck Plus. Board-specific
  profiles and USB-port notes are in [BUILDING.md](BUILDING.md).

At the end of this step the node boots, but it is not yet part of any mesh.
Step 3 explains why.

## 2. Connect the web client

The [web client](../webapp/README.md) is where you provision and operate a
node. It connects three ways:

- **WiFi (WebSocket).** Enter the node's address (for example
  `ws://192.168.1.100/ws`) and its auth token in the WiFi connection dialog.
- **USB (serial).** A direct serial connection. Serial needs no auth token by
  design: physical access to the device is the trust bootstrap.
- **BLE.** Bluetooth to a nearby node; the app remembers the per-device token
  in its device book.

Where the auth token comes from: on first boot the firmware generates a random
token and stores it in NVS. WiFi and BLE require it; retrieve it over USB with
`bramble pair`, or read it from the serial boot log the first time it is
generated. Full detail, including the browser origin allowlist and how to set
or disable a token, is in [auth.md](auth.md). Enter the token in the **Auth
Token** field of the WiFi connection dialog.

## 3. Why the node is silent: fail-closed by default

A freshly flashed node is **unprovisioned and inert**. There is no built-in
default network key, so the node fails closed: `network_key_get()` fails,
every control-plane verifier (RREP, RERR, ACK, delivery receipt, beacon)
rejects before comparing, and the node neither emits nor accepts authenticated
control-plane traffic. It will not mesh until you give it a real per-fleet key.

You do not have to guess at this. When you connect the web client to an
unprovisioned node it shows a prominent banner across the top of the app:

> This node is UNPROVISIONED and inert. It has no network key, so it is not
> meshing. Generate a key here to found a network, or paste one from an
> existing node to join.

The banner has a **Provision** button that jumps to the Config tab. It
disappears the moment a key is set. This fail-closed posture is deliberate:
an unprovisioned node cannot be tricked into forging or accepting control
traffic. See [network-key-provisioning.md](network-key-provisioning.md) for
the full behavior.

## 4. Provision a network key

The network key is the shared symmetric key behind the control-plane HMACs.
Provisioning is the step that takes a node from inert to meshing. Open
**Config -> Network Key**:

- **Found a new network** (your first node): click **Generate key**. This
  mints a random 32-byte key in the browser and provisions it on this node
  immediately, making it the founder. Note the fingerprint
  (`SHA256(key)[0:4]`, 8 hex chars) and record the key out of band (copy the
  hex or save the QR): the key is never recoverable from a device afterward.
- **Join an existing network** (a later node): under **Join an existing
  network**, click **Scan QR** to scan the founder's key, or paste its
  `bramble://net/v1?k=...` string (or the bare 64 hex chars) and submit. This
  calls `bramble.setNetworkKey` on the connected node.

After provisioning, the Network Key section refreshes to
`Provisioned (fingerprint XXXXXXXX)`. Confirm every node in the fleet reports
the **same** fingerprint as the founder; compare fingerprints over a channel
you trust, the same way you would compare any shared secret. A node still
reporting `Unprovisioned` is not part of the authenticated control plane. The
full provisioning and convergence procedure is in
[network-key-provisioning.md](network-key-provisioning.md).

What a shared network key does and does not buy you: it excludes outsiders
and closes replay of captured control messages, but every holder of the key
is an insider and can still forge control-plane MACs on behalf of any other
holder. The key proves fleet membership, not per-node identity. Step 5 is how
you narrow that further.

## 5. Optional: set up a trust anchor and enroll the node

A network key admits members but does not stop a member from minting extra
identities (a Sybil). If you want to close Sybil identity minting on your
fleet, set up a per-fleet **trust anchor**: an operator-held Ed25519 keypair
that endorses each member's identity. An anchored node only pins peers
carrying a cert the anchor signed, so an un-admitted Sybil cannot get pinned.
This is opt-in per fleet.

The short version, from **Config -> Trust Anchor**:

1. **Generate anchor** draws a fresh seed in your browser and shows a backup
   string. The seed is not saved or used until you click **I have saved this
   backup**, so record the backup offline first. The seed never leaves your
   browser and is never sent to a node.
2. **Provision anchor to this node** sends the anchor's public key to the node
   (`bramble.setAnchor`).
3. **Enroll this node** reads the node's identity key (`bramble.getIdentity`),
   signs a cert locally, and applies it (`bramble.setEndorsement`); the status
   refreshes to Endorsed. For a node you are not connected to, use **Show my
   identity** on that node and **Sign cert** / **Apply an endorsement** to move
   the identity out and the cert back as share strings.

Honest residuals, in the same breath: a compromised endorsed insider stays a
valid member (endorsement is admission control, not behavioral trust), the
anchor seed is the fleet's trust root and whoever holds it can admit any node,
and v1 certs are permanent with no active revocation. The full ceremony,
custody duties, and residuals are in [trust-anchor.md](trust-anchor.md).

## 6. Send your first message and read the delivery status

With the node provisioned (and, on a fleet, at least one peer on the same key),
open the Chat view and send a message. Each sent message carries a delivery
status indicator so you know what actually happened rather than fire-and-forget:

- **Queued** / **Sending** / **Sent to next hop**: the message is in flight and
  not yet confirmed.
- **Delivered**: the acknowledged tiers (Normal and Critical) confirmed
  delivery back to you.
- **Failed**: the message was not delivered after its retries.
- **No confirmation yet**: sent, but no acknowledgement has arrived (expected
  for broadcast, which is fire-and-forget).

Delivery confirmation applies to the acknowledged reliability tiers; broadcast
messages are fire-and-forget and are not individually confirmed. See
[webapp/chat.md](webapp/chat.md) for current chat behavior and
[SECURITY-MODEL.md](SECURITY-MODEL.md) for what is and is not protected.

That is a first node from zero to a confirmed message. From here,
[network-key-provisioning.md](network-key-provisioning.md) and
[trust-anchor.md](trust-anchor.md) cover fleet-wide provisioning, and
[SECURITY-MODEL.md](SECURITY-MODEL.md) is the honest threat model.
