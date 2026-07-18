# Network Key Provisioning

The network key is the symmetric key behind the control-plane HMACs on
RREP, RERR, ACK, delivery receipt, and beacon packets (see
`docs/bramble-protocol-spec.md` §4.25 items 5-9, and
`docs/SECURITY-MODEL.md` §3 for what it does and does not protect). This
guide is the operator-facing steps for setting it across a fleet. It
assumes you already have RPC auth set up (`docs/auth.md`); provisioning a
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

## Steps

1. **Generate a key.** In the webapp, open **Config → Network Key** and
   click **Generate key**. This creates a random 32-byte key in your
   browser; it is not sent anywhere yet. Note the fingerprint shown next
   to it (`SHA256(key)[0:4]`, 8 hex chars) so you can confirm convergence
   later.

2. **Record the key out-of-band.** Copy the hex value or save the QR code
   somewhere durable (password manager, printed backup). The key is
   **never recoverable from a device**: `bramble.getNetworkKeyStatus`
   reports only whether a node is provisioned and its fingerprint, never
   the key itself. If you lose the recorded key, you cannot retrieve it
   from a node you already provisioned; generate and distribute a new one
   instead.

3. **Provision each node**, one at a time, while connected to that node:
   - Scan the QR code with **Provision → Scan QR**, or
   - Paste the `bramble://net/v1?k=...` string (or the bare 64 hex chars)
     into the paste field and click **Provision**.

   This calls `bramble.setNetworkKey` on the connected node. It takes
   effect live for RREP, RERR, ACK, and delivery-receipt verification
   immediately, and also re-derives the beacon HMAC key live (no reboot
   required).

4. **Confirm convergence.** After provisioning, the Network Key section
   refreshes and shows `Provisioned (fingerprint XXXXXXXX)`. Repeat this
   check on every node in the fleet and compare fingerprints: they must
   all match the fingerprint from step 1. **A node still reporting
   `Unprovisioned` (the all-zero fingerprint sentinel) is not part of the
   authenticated control plane**: it neither emits nor accepts
   control-plane MACs, so it cannot route for the fleet until it is
   provisioned, regardless of what the rest of the fleet is running.

## What a matching fingerprint proves, and what it does not

A matching `SHA256(key)[0:4]` fingerprint across two nodes proves they
hold the same network key, without either node ever transmitting the key
itself over RPC a second time. It does **not** authenticate that you are
talking to the node you think you are (it is not a short-authentication-
string handshake), and it does not prove anything about messages already
in flight before provisioning finished. Compare fingerprints over a
channel you trust (in person, or the same secure channel you used to
distribute the key), the same way you would compare any shared secret.

## Storage

The network key is stored as a plaintext NVS entry on each device, the
same as the RPC auth token and channel PSKs
(`docs/SECURITY-MODEL.md` known gaps). Flash encryption for this class of
secret is a separate mechanism (see [security/keys-at-rest.md](security/keys-at-rest.md))
and is not part of this provisioning flow.
