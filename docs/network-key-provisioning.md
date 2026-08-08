# Network Key Provisioning

The network key is the symmetric key behind the control-plane HMACs on
RREP, RERR, ACK, delivery receipt, and beacon packets (see
`docs/bramble-protocol-spec.md` §4.25 items 5-9, and
`docs/SECURITY-MODEL.md` §3 for what it does and does not protect). This
guide is the operator-facing reference for setting it across a fleet. For
the guided first-mesh walkthrough with screenshots, start at
[getting-started.md](getting-started.md); this document is the reference
that covers every surface and the fleet-scale procedure.

It assumes you already have RPC auth set up (`docs/auth.md`); provisioning a
network key uses the same authenticated-RPC gate as `setAuthToken`.

## Before you start

**Every node ships unprovisioned, and an unprovisioned node is inert on
the control plane.** There is no fallback key: `network_key_get()` fails
closed, MAC emission writes an all-zero sentinel, and every control-plane
verifier (RREP, RERR, ACK, delivery receipt, beacon) rejects before
comparing. An outsider cannot forge control traffic against an
unprovisioned node; the node simply does not participate in the
authenticated control plane until you provision it. (The compile-time
`BRAMBLE_PUBLIC_CHANNEL_PSK` constant is used only by the public
broadcast channel, never the control plane.)

**What provisioning does and does not give you.** After every node in
your fleet is provisioned on the same key:

- a holder of that key can still forge control messages on behalf of any
  other holder (the key proves fleet membership, not per-node identity);
- replay of captured control messages is closed: all five MACs bind a
  monotonic 48-bit origin sequence, checked against a per-signer replay
  window;
- on an **anchored** mesh (see `docs/trust-anchor.md`), Sybil identity
  minting is also closed; on an un-anchored mesh, a key holder can still
  mint additional identities at will.

See `docs/SECURITY-MODEL.md` §3 and §5 for the full picture, including
why none of this claims a short-authentication-string comparison or
forward secrecy for the network key.

## The two paths: found, then join

Provisioning splits into founding a network once and joining every other
node to it.

**Found** mints a new key. `bramble.generateNetworkKey` draws an
entropy-gated 32-byte key **on the device**, provisions that node with it
atomically (RAM and NVS), re-derives the beacon HMAC key live, and returns
the raw key exactly once. That node is the founder. On entropy failure it
provisions nothing and returns an error, leaving any previous state
untouched.

**Join** applies an existing key. `bramble.setNetworkKey` takes the 64-hex
key and provisions the node with it, live, no reboot.

The key is write-only at the device boundary. Nothing reads a provisioned
key back: `bramble.getNetworkKeyStatus` reports only whether a node is
provisioned and the key's fingerprint. **The copy returned when you found
the network is the only copy that will ever exist.** Record it out of band
(password manager, printed backup) before you rely on it. If you lose it,
you cannot recover it from a node that holds it; you can only found a new
network and re-join every node.

## Fingerprints, and what they prove

`SHA256(key)[0:4]`, rendered as 8 lowercase hex characters, is the
fingerprint every node reports. An unprovisioned node reports the all-zero
sentinel `00000000`.

A matching fingerprint across two nodes proves they hold the same network
key, without either node ever transmitting the key again. It does **not**
authenticate that you are talking to the node you think you are (it is not
a short-authentication-string handshake), and it does not prove anything
about messages already in flight before provisioning finished. Compare
fingerprints over a channel you trust (in person, or the same secure
channel you used to distribute the key), the same way you would compare any
shared secret.

## Provisioning surfaces

Four surfaces reach the same three RPCs. Pick whichever fits; they are
interchangeable, and a fleet can mix them.

| Surface | Found | Join | Notes |
| --- | --- | --- | --- |
| Web app, **Config → Network Key** | yes | yes | The full surface: QR display and scan, re-key confirmation, live fingerprint. |
| Web flasher, **Device Setup** | no | yes | Join-only, over the serial link already open from flashing. |
| Raw RPC | yes | yes | `bramble.generateNetworkKey`, `bramble.setNetworkKey`, `bramble.getNetworkKeyStatus`. |

The web flasher does not offer founding on purpose. Minting a fleet's root
secret belongs where the QR code, the copy-confirm, the persistent
fingerprint readout, and the re-key guard live; a one-shot page you close
cannot offer those, and the key is unrecoverable afterwards.

## Fleet procedure

1. **Found the network on one node.** In the web app, **Config → Network
   Key → Found a new network → Generate key**. Record the key and note the
   fingerprint.

2. **Join every other node.** Scan the QR or paste the key into **Join an
   existing network**, or paste it into the web flasher's Network Key field
   while flashing.

3. **Confirm convergence.** Check every node reports the founder's
   fingerprint on the Network Key section's Status line.
   **A node still reporting `Unprovisioned` is not part of
   the authenticated control plane**: it neither emits nor accepts
   control-plane MACs, so it cannot route for the fleet, regardless of what
   the rest of the fleet is running.

## Re-keying

Provisioning a different key on a node that already has one re-keys it and
cuts it off from every node still on the old key. The web app makes you
confirm before doing this. There is no fleet-wide rekey operation: re-keying
a fleet means provisioning the new key on every node, and the fleet is
partitioned until you finish. Nodes on the old key and nodes on the new key
will not route for each other.

## Storage

The network key is stored as a plaintext NVS entry on each device, the
same as the RPC auth token and channel PSKs
(`docs/SECURITY-MODEL.md` known gaps). Flash encryption for this class of
secret is a separate mechanism (see [security/keys-at-rest.md](security/keys-at-rest.md))
and is not part of this provisioning flow.
