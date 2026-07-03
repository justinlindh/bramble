# Network Key Provisioning

The network key is the symmetric key behind the control-plane HMACs on
RREP, RERR, ACK, delivery receipt, and beacon packets (see
`docs/bramble-protocol-spec.md` §4.25 items 5-9, and
`docs/SECURITY-MODEL.md` §3 for what it does and does not protect). This
guide is the operator-facing steps for setting it across a fleet. It
assumes you already have RPC auth set up (`docs/auth.md`); provisioning a
network key uses the same authenticated-RPC gate as `setAuthToken`.

## Before you start

**Every node ships unprovisioned**, using a key derived from a public,
compile-time constant checked into this repository
(`BRAMBLE_PUBLIC_CHANNEL_PSK`). Anyone who has read the Bramble source
can compute that fallback key and forge a valid RREP, RERR, ACK, delivery
receipt, or beacon HMAC against any unprovisioned node. Until you
provision a real key, treat the control plane as **integrity-only**
(proves "this is Bramble-compatible code," not "this is a fleet member")
rather than authenticated.

**Provisioning does not, by itself, close SEC-H1, SEC-H2, NEW-SEC-4, or
NEW-SEC-8.** After every node in your fleet is provisioned on the same
key:

- a holder of that key can still forge control messages on behalf of any
  other holder (the key proves fleet membership, not per-node identity);
- a captured, genuinely-valid RREP, RERR, ACK, delivery receipt, or beacon
  can still be replayed, since none of the five MACs carries a freshness
  or sequence check yet (tracked follow-on: per-message freshness,
  RERR-replay-into-live-teardown is the worst case);
- the NEW-SEC-4 timesync bootstrap race is still open: one key holder can
  still satisfy the corroboration quorum under multiple fabricated source
  addresses (tracked follow-on: per-node beacon identity).

Provisioning closes the "no distribution mechanism, universally-known
fallback key" hole. It is the foundation the freshness and per-node
identity follow-on work builds on, not a substitute for it. See
`docs/SECURITY-MODEL.md` §3 and §5 for the full picture, including why
none of this claims a short-authentication-string comparison or forward
secrecy for the network key.

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
   required, as of this batch).

4. **Confirm convergence.** After provisioning, the Network Key section
   refreshes and shows `Provisioned (fingerprint XXXXXXXX)`. Repeat this
   check on every node in the fleet and compare fingerprints: they must
   all match the fingerprint from step 1. **A node still reporting the
   public-fallback fingerprint is unprotected** and will keep accepting
   forged control-plane traffic from anyone who knows the compile-time
   fallback constant, regardless of what the rest of the fleet is running.

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
secret is tracked separately (workstream 1.5) and is not part of this
provisioning mechanism.
