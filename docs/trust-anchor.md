# Trust Anchor Enrollment

The trust anchor is the fleet's membership-authority root. Every node
already has an unforgeable per-node Ed25519 identity (address =
SHA-256(key)[0:4], self-signed attestations, TOFU pinning, address-key
binding: see `docs/SECURITY-MODEL.md` section 3, "Per-node cryptographic
identity"). Identity answers "is this the same node I saw before"; it does
NOT answer "is this node allowed in my fleet". On an un-anchored mesh
identities are unforgeable but **free to mint**, so a Sybil can spin up
arbitrarily many valid identities. The trust anchor closes that: an
operator holds one anchor keypair per fleet, provisions its PUBLIC key to
every node, and signs an endorsement cert over each node's identity key.
An anchored node only pins peers that carry a cert the anchor signed, so a
Sybil with fresh identities cannot get pinned. This guide is the
operator-facing enrollment ceremony.

It assumes you already have a provisioned network key (`docs/network-key-
provisioning.md`) and RPC auth (`docs/auth.md`); anchor provisioning and
endorsement use the same authenticated-RPC gate.

## The model

- **One anchor per fleet.** The anchor is an Ed25519 keypair. Its PRIVATE
  seed (32 bytes) is the fleet's root of trust; its PUBLIC key is what
  nodes pin to.
- **The private seed is operator-held and offline.** It lives ONLY in the
  operator's browser (localStorage, under `bramble.anchor.seed`) and is
  **never sent to a node or over any RPC**. Provisioning sends the public
  key; enrollment signs a cert locally and sends only that cert. There is
  no RPC that returns or accepts the seed.
- **Endorsement certs are per-node and public.** A cert is the anchor's
  Ed25519 signature over `("bramble-endorse-v1" || node_ed25519_pub ||
  not_after)`. It is not secret: it travels to the node it endorses and is
  useless for anything else. v1 always issues `not_after = PERMANENT`
  (0xFFFFFFFFFFFFFFFF); see "Consequences and custody duties" for what
  permanent means.
- **The node self-verifies.** A node accepts a cert only if it verifies
  against the node's own identity key AND the anchor public key the node
  is provisioned with. A cert for the wrong node, or signed by the wrong
  anchor, is rejected by the firmware; the webapp surfaces that rejection.

## The enrollment ceremony

### 1. Generate and BACK UP the anchor

In the webapp, open **Config -> Trust Anchor** and click **Generate
anchor**. This draws a fresh 32-byte seed in your browser and shows its
backup string (`bramble://anchor/v1?sk=...`) as copyable text and a QR,
with its fingerprint (`SHA256(anchor_pub)[0:4]`, 8 hex chars).

**Backup is mandatory and gated.** The seed is NOT persisted and NOT sent
anywhere until you click **I have saved this backup**. Save the backup
string somewhere durable and offline (password manager, printed QR) first.
Cancelling discards the anchor entirely. This gate exists because losing
the seed with no backup is the worst operator outcome (see below).

To move an existing anchor to another browser, paste its
`bramble://anchor/v1?sk=...` backup into **Import anchor backup**; the
webapp recomputes the public key and shows the fingerprint so you can
confirm it is the right anchor.

### 2. Provision the anchor public key to each node

Connected to a node, click **Provision anchor to this node**. This calls
`bramble.setAnchor` with the anchor PUBLIC key only. The **This node**
panel then shows the node's anchor status: whether it is anchored, to
which fingerprint, and whether it is endorsed. If the node is already
anchored to a DIFFERENT fingerprint than the anchor in your browser, the
webapp shows a MISMATCH warning: a cert signed by your anchor would be
rejected by that node until you re-provision it.

Repeat on every node. Confirm each reports the same anchor fingerprint as
step 1, the same way you confirm network-key convergence.

### 3. Enroll each node

Enrolling a node means signing a permanent cert over its identity key and
applying it. Two paths, both keeping the anchor seed in your browser:

**Local (the connected node).** Click **Enroll this node**. The webapp
reads the node's `ed25519_pub` via `bramble.getIdentity`, signs a
permanent cert locally, and applies it via `bramble.setEndorsement`. The
node self-verifies and the status refreshes to **Endorsed (enrolled)**.
The button is disabled if you hold no anchor, or if the node is anchored
to a different fingerprint (enrolling would mint a dead cert).

**Remote (a node you are not connected to).** The remote node's operator
opens **Config -> Trust Anchor -> Show my identity** and sends you the
resulting `bramble://ident/v1?pk=...` identity share (public key, not
secret) over any channel. You paste it under **Enroll a remote node ->
Sign cert**; the webapp signs a permanent cert and produces a
`bramble://endorse/v1?na=...&sig=...` cert share (copyable + QR). Send that
back. The remote operator pastes it under **Apply an endorsement**, which
calls `bramble.setEndorsement` on their node. The anchor seed never leaves
your browser at any point in this exchange.

## Local vs remote, and the share codecs

Three share strings carry the ceremony (`webapp/src/utils/anchorShare.ts`):

| Scheme | Carries | Secret? | Direction |
|---|---|---|---|
| `bramble://anchor/v1?sk=` | anchor seed | **YES** | offline backup only, never to a node |
| `bramble://ident/v1?pk=` | a node's identity pubkey | no | node -> anchor operator |
| `bramble://endorse/v1?na=&sig=` | an endorsement cert | no | anchor operator -> node |

Local enrollment reads the identity and applies the cert over the live RPC
connection to the one node in front of you. Remote enrollment moves the
identity out and the cert back as share strings over whatever channel you
trust, so one offline anchor holder can enroll a whole fleet without ever
connecting to each node. The seed scheme exists ONLY for operator backup;
nothing on a node ever emits or accepts it.

## What this closes, and what it does not

Anchoring closes **Sybil scarcity on an anchored mesh**: pinning now
requires an anchor endorsement a Sybil cannot forge, so an outsider or an
un-admitted Sybil cannot get pinned, cannot join the identity-gated
timesync quorum, and cannot pass DM key-continuity checks. This is the
NEW-SEC-4 close described in `docs/SECURITY-MODEL.md` section 5.

It does NOT protect against:

- **A compromised admitted insider.** A node you endorsed that is later
  captured or turns malicious stays a valid member. Endorsement is
  admission control, not behavioral trust; a keyed insider's forgeries and
  routing lies remain in scope exactly as before (SECURITY-MODEL section 1
  and the control-plane residuals).
- **Anchor-holder compromise.** The anchor seed IS the trust root. Whoever
  holds it can admit any node. Guard it like the fleet's master key; its
  custody is the whole security of the scheme.
- **Un-anchored meshes.** A node with no anchor provisioned falls back to
  the prior identity model (unforgeable but free-to-mint identities, TOFU
  pinning). Anchoring is opt-in per fleet.

See `docs/SECURITY-MODEL.md` sections 3 and 5 for the full residual list;
this doc does not duplicate it.

## Consequences and custody duties

- **Back up the seed, or lose the ability to enroll.** The seed is
  recoverable only from your own backup; no node stores or returns it. If
  you lose it with no backup, you cannot sign new certs: existing endorsed
  nodes keep working, but you can never admit another node without
  **re-anchoring** the whole fleet (generate a new anchor, re-provision its
  public key everywhere, re-enroll every node). That is a flag day.
- **A leaked seed is a Sybil risk until you re-anchor.** Anyone with the
  seed can enroll arbitrary identities into your fleet. There is no way to
  invalidate a leaked anchor short of re-anchoring.
- **Certs are permanent in v1; there is no active revocation.** A cert is
  issued with `not_after = PERMANENT` and stays valid until the fleet
  re-anchors or excludes the node out of band. A compromised endorsed node
  cannot be individually un-trusted fleet-wide in v1 short of a re-anchor.
  Time-bounded certs and revocation are possible future work (the wire
  already carries `not_after`), not a shipped capability.

## Storage

The anchor seed is stored as a plaintext localStorage entry in the
operator's browser, the same client-side storage class as other
`bramble:*` client state. It is deliberately NOT stored on any node.
Treat the browser profile holding it as sensitive, and rely on the
mandatory offline backup as the durable copy.
