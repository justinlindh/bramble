# Attested roll-call

A roll-call answers one fleet-level question: **did this reach everyone, and
can each answer be proven?**

An initiator floods a short operator payload as an authenticated broadcast.
Every member that hears it answers with a unicast carrying an Ed25519
signature bound to that roll-call and to the responder's own identity key. The
initiator accumulates a ledger: who answered, how far into the roll-call, over
which relay path, and (on an anchored mesh) which admitted members did not.

The primitive exists because a broadcast on a mesh is a hope, not a fact. A
delivery receipt tells you a frame arrived somewhere; a roll-call tells you
which named members were alive, in range, and willing to say so, with a
signature you can check afterwards.

![The web client's Roll Call panel: a roll-call captioned "sound off",
collecting with 2m 6s left on round 1 of 3, reporting 4 of 5 expected on an
anchored fleet, a table of the four members that answered with the time into
the roll-call, the round they named and their relay path where one was
reported, and a red "No answer (1)" line naming the member that stayed
silent](images/webapp-rollcall.png)

## How the frames move

Neither direction adds a transport. Both ride the ordinary channel DATA
envelope, separated only by an inner app type:

| | Frame | Path |
| --- | --- | --- |
| Announce | broadcast DATA, `APP_TYPE_ROLLCALL` | channel-key AEAD, the network-key `auth_hmac` every DATA carries, the shared flood relay, the BROADCAST airtime lane |
| Answer | unicast DATA, `APP_TYPE_ROLLCALL_REPLY` | the normal reactive path, registered in the pending-ACK table at `MSG_TIER_NORMAL` like any other unicast |

So a roll-call inherits the flood suppression, the replay window, the
budget-gated TX path and the retry ladder that already exist. There is no
second flood, no second authenticator, and no roll-call timer: the re-announce
rounds and the staggered answers are driven from the mesh task's existing
maintenance tick.

The answer's signature covers a canonical message with a context prefix that
domain-separates it from every other Ed25519 use in the tree:

```text
"bramble-rollcall-v1" || rollcall_id(4) || initiator_addr(4) || responder_addr(4)
```

The initiator is inside the signature, so an answer captured from one
operator's roll-call cannot be replayed into another operator's roll-call that
happened to draw the same id. The responder address is carried explicitly on
the wire as well as inside the signature, and the initiator rejects an answer
whose two disagree: that combination is somebody relaying another node's
answer under their own envelope, which is an answer from neither of them.

## Anchored and un-anchored fleets

The ledger reports one of two fundamentally different things, and says which.

**Anchored.** The node holds a provisioned fleet anchor, so every pinned
identity was endorsed by that anchor: "pinned" and "admitted to this fleet"
are the same set. That set is the expected set, and the ledger reports
`M of N expected` and names the members that did not answer.

**Un-anchored.** The node pins trust-on-first-use identities, which are free
to mint. There is no authoritative roster, so the ledger reports the
responders it observed, `expected` is 0, and `missing` is empty by
construction. This is a deliberate bound, not an oversight: calling an address
missing implies you knew it should have been there, and an un-anchored mesh
does not.

Both the RPC (`anchored`) and the web client (an "Anchored fleet" or "Observed
only" label) carry the distinction explicitly, so a screenshot of a ledger can
never be mistaken for a complete fleet answer when it is not one.

See [trust-anchor.md](trust-anchor.md) for the enrollment ceremony.

## Airtime cost, and how it scales

A roll-call is the most expensive primitive in the protocol, so its cost is
bounded by construction rather than by operator discipline.

Wire sizes, at the shipped envelope (28-byte DATA prefix + 12-byte nonce +
8-byte channel header + payload + 16-byte tag):

| Frame | On-air bytes |
| --- | --- |
| Announce, empty payload | 70 |
| Announce, full 48-byte payload | 118 |
| Answer | 137 |

Per roll-call, with N members and a mesh of R relaying nodes:

- **3 announce floods** (`ROLLCALL_MAX_ROUNDS`), each costing one origination
  plus up to R relayed transmissions. Round 2 goes out 30s after round 1,
  round 3 60s after round 2.
- **N answers, not 3N.** A member answers a given roll-call at most once, no
  matter how many rounds it hears, so rounds 2 and 3 cost a decode on a member
  that already answered and nothing more. Each answer costs one transmission
  per hop back to the initiator, plus the ACK the reliability layer already
  sends.

The dominant term is therefore `N` unicast answers of 137 bytes each, times
their hop counts: a roll-call over a 20-node mesh averaging 2 hops is roughly
40 answer transmissions plus 3 floods. That is affordable occasionally and
ruinous continuously, which is why initiation is rate limited.

The answers are slotted rather than fired at once. Each member draws a
deterministic bucket from its own address XOR the roll-call id, over a window
of two buckets per known peer clamped to [4, 32], and answers at
`400ms + slot × 600ms + 0..500ms` of jitter. Address-derived so two nodes
rarely collide; id-derived so the same two do not collide again on the next
roll-call; peer-count-sized so a dense mesh spreads wider than a sparse one.
The widest window is about 19.5s.

The collection window is fixed at 135s (the two round gaps plus a 45s tail),
after which the ledger closes. A later answer is counted as `late` but does
not reopen it, so "closed" is a stable, reportable state.

## Bounds

| Bound | Value | What happens past it |
| --- | --- | --- |
| Initiation interval | 5 minutes, start to start | Refused with `reason: rate_limited` and the milliseconds to wait |
| Concurrent roll-calls started here | 1 | Refused with `reason: busy` and the milliseconds until the current one closes |
| Operator payload | 48 bytes | Rejected as invalid params |
| Tracked responders | 24 | Counted in `overflow` rather than dropped silently, so a ledger that could not hold the whole mesh says so |
| Announce rounds | 3 | No further rounds; the schedule is a hard airtime bound |
| Answers a member owes at once | 2 concurrent initiators | Counted in `pending_dropped`, reported by that node, so a node that failed to take part says so locally instead of only appearing as a hole in someone else's ledger |

The rate limit is enforced in the firmware, not in the client: it has to hold
for a script driving the RPC directly.

## What a roll-call proves, and what it does not

**A verifying answer proves** the holder of that address's pinned identity key
was alive, heard this exact roll-call, and chose to answer. It cannot be
minted by another network-key insider on that member's behalf: the signature
is over the responder's own key, and the initiator checks it against the pin
it already holds.

**A missing answer proves nothing on its own.** The member may be switched
off, out of radio range, out of airtime budget, or simply not answering. A
relay that holds the network key can also drop the announce on its way out or
the answer on its way back: it cannot forge an answer, but it can withhold
one. Every ledger is therefore evidence of presence, never of absence, and the
web client says so next to the numbers.

An answer from a peer this node holds no pinned key for cannot be attested at
all. It is counted in `unattested` and never recorded as a responder, because
a response nobody can check is not evidence.

Out of scope, so that nothing here reads as a promise: a roll-call does not
integrate with the store-and-forward mailbox. An offline member is reported
missing, full stop, and no answer is delivered later when it rejoins.

## RPC

`bramble.startRollCall` takes an optional `text` (up to 48 bytes) and returns
`ok: true` with the roll-call id, the collection window and the expected-set
size. An operational refusal comes back as `ok: false` with a `reason`
(`busy`, `rate_limited`, `not_transmitted`) and `retry_after_ms`, so a caller
waits a known interval instead of polling. A payload over the cap is a
malformed request and returns `-32602`.

`bramble.getRollCall` returns the ledger of the roll-call this node started:
the per-member rows (address, whether an answer verified, milliseconds INTO
the roll-call, the round it named, the relay path where the broadcast
delivery-receipt machinery supplied one), the missing set, and the honest
counters (`unattested`, `overflow`, `late`, `pending_dropped`). Times are
reported as milliseconds into the roll-call, never as device uptime: the
device clock is boot-relative and means nothing to a client, while "answered
4.2s in" is directly comparable across nodes.

Progress also arrives as notifications: `bramble.onRollCall` on a member that
heard an announce, `bramble.onRollCallResponse` on the initiator per verified
answer, and `bramble.onRollCallComplete` once when the window closes. Full
schemas are in [`api/openapi.yaml`](../api/openapi.yaml); the notification
reference is [rpc/events.md](rpc/events.md).

`bramble-go` and `bramble-cli` do not speak these methods.

## How this is verified

- `test/test_rollcall.c` covers the pure core against the shipped codecs: the
  wire round trips and their malformed-input rejections, the signed-message
  layout, a second fleet member failing to forge an answer on a victim's
  behalf, the stagger bounds, the round schedule, the rate limit across the
  millisecond rollover, and the ledger's idempotence, close, overflow and
  missing-set rules.
- `test/test_rpc_rollcall.c` drives both RPC methods through the real
  dispatcher and the real ledger, including the un-anchored ledger that names
  nobody missing.
- `simulator/gosim/rollcall_test.go` runs a roll-call end to end over the
  collision-modeled simulator on a 4-node line, one to three hops out: every
  member answers once and attests, and the ledger closes 3 of 3. A second
  scenario partitions a pinned member and asserts the ledger names exactly
  that address missing. These are simulation results, not bench measurements.
- The screenshot above is a real capture, not a mock-up:
  `webapp/scripts/capture-rollcall-shot.mjs` starts the unified server (which
  serves the built web client and the embedded mock node), provisions and
  anchors that node over its own RPC surface, drives the real UI through
  starting a roll-call in headless Chromium, waits for the mock fleet's
  answers to land, and screenshots the panel. The five peers, their answer
  times and the one that never answers are fixed in `webapp/mock/handler.mjs`
  so the image is reproducible. The addresses and names shown are the mock's
  documentation placeholders.
