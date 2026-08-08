#ifndef BRAMBLE_ROLLCALL_H
#define BRAMBLE_ROLLCALL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Attested roll-call: the pure protocol core.
 *
 * A roll-call answers one fleet-level question: "did this message reach
 * everyone, and can each answer be proven?". An INITIATOR floods a short
 * operator payload as an authenticated broadcast; every receiving MEMBER
 * answers with a unicast reply carrying an Ed25519 signature bound to
 * (roll-call id, initiator, responder), so a response cannot be minted by
 * another network-key insider on someone else's behalf. The initiator
 * accumulates a LEDGER: who answered, when, over which relay path, and (on
 * an anchored mesh) which anchor-certified peers did not.
 *
 * This header is the whole primitive minus its transport: the wire codecs,
 * the signed-message canonicalization, the response stagger, the round
 * schedule, the initiation rate limit, and the ledger. It owns no state, no
 * clock, no radio and no crypto backend selection, so the firmware
 * (main/mesh_rollcall.c), the host suites (test/test_rollcall.c) and the Go
 * simulator (simulator/gosim) all drive the SAME logic rather than three
 * re-expressions of it.
 *
 * TRANSPORT (owned by the caller, deliberately not by this file): both
 * frames ride the existing channel-encrypted DATA envelope under new inner
 * app types, so they inherit the network-key auth_hmac, the flood relay, the
 * reactive unicast path, the reliability tiers and the budget-gated tx_gate
 * unchanged. There is no second flood path and no second authenticator.
 *
 * WHAT A SIGNATURE PROVES, AND WHAT ABSENCE DOES NOT: a verifying response
 * proves the holder of that address's pinned identity key was alive, heard
 * this exact roll-call, and chose to answer. A MISSING response proves
 * nothing on its own: the member may be off, out of range, out of airtime
 * budget, or a network-key insider may have suppressed or declined it. The
 * ledger is evidence of presence, never of absence.
 */

/* ── Sizes and bounds ────────────────────────────────────────────────── */

/* Operator payload carried by the announce. Deliberately short: the announce
 * is a flood, and every byte is paid for once per relay per round. */
#define ROLLCALL_TEXT_MAX 48

/* Re-announce rounds per roll-call, including the first. Bounded so the
 * primitive's worst-case airtime is a fixed multiple of a single flood
 * rather than an operator-tunable one. */
#define ROLLCALL_MAX_ROUNDS 3

/* Ledger capacity. Responders past the cap are counted (see
 * rollcall_ledger_t.overflow) rather than silently dropped, so a ledger that
 * could not hold the whole mesh says so instead of reading as "these are all
 * the nodes that answered".
 *
 * The table holds two kinds of row and they are NOT equal citizens. An
 * ATTESTED answer is the ledger's whole output; a relay path harvested from
 * the delivery-receipt machinery is decoration, and it is authenticated only
 * by the SHARED network key, so its source address is anything an insider
 * cares to write. A path may therefore never cost an answer a slot: see
 * rollcall_ledger_note_path's contract and the reclaim rule in
 * rollcall_ledger_note_response. */
#define ROLLCALL_MAX_RESPONDERS 24
#define ROLLCALL_MAX_EXPECTED 24

/* Relay path depth, matching DELIVERY_RECEIPT_MAX_HOPS: the path a ledger
 * row can carry is exactly the path the delivery-receipt machinery can
 * deliver, so nothing is truncated on the way in. */
#define ROLLCALL_PATH_MAX 8

/* Member-side "already answered" table. A member answers each roll-call at
 * most once no matter how many rounds it hears; this is the memory that
 * makes the re-announce rounds idempotent. Sized for the number of DISTINCT
 * roll-calls a node might see in flight at once (in practice one). */
#define ROLLCALL_SEEN_MAX 8

/* Answers a member may owe at once, one per concurrent INITIATOR: a member
 * answers a given roll-call at most once (rollcall_seen_claim), so this
 * queue only ever holds roll-calls from different initiators. Two is the
 * honest bound; a third concurrent initiator is refused and counted rather
 * than silently displacing an answer already owed. Lives here rather than in
 * the firmware glue so the simulator's member state is sized by the same
 * constant. */
#define ROLLCALL_PENDING_MAX 2

/* ── Wire ────────────────────────────────────────────────────────────── */

/*
 * Announce payload (inner plaintext of a broadcast channel DATA frame,
 * app type APP_TYPE_ROLLCALL):
 *
 *   rollcall_id(4, big-endian) || round(1) || text_len(1) || text(text_len)
 *
 * round is 1-based and identifies WHICH re-announce this frame is, so a
 * response can name the round it answered. text is the operator payload; it
 * is NOT NUL-terminated on the wire.
 */
#define ROLLCALL_ANNOUNCE_HEADER_SIZE 6
#define ROLLCALL_ANNOUNCE_MAX_SIZE (ROLLCALL_ANNOUNCE_HEADER_SIZE + ROLLCALL_TEXT_MAX)

/*
 * Response payload (inner plaintext of a UNICAST channel DATA frame back to
 * the initiator, app type APP_TYPE_ROLLCALL_REPLY):
 *
 *   rollcall_id(4, big-endian) || round(1) || responder_addr(4, big-endian)
 *   || sig(64)
 *
 * responder_addr is carried explicitly even though the envelope also has a
 * src_addr: the signature covers THIS field, and a verifier must check the
 * bytes that were signed rather than a field some other layer supplied.
 * The caller cross-checks the two and rejects a mismatch.
 */
#define ROLLCALL_SIG_SIZE 64
#define ROLLCALL_RESPONSE_SIZE (4 + 1 + 4 + ROLLCALL_SIG_SIZE) /* 73 */

/*
 * Canonical signed message:
 *
 *   "bramble-rollcall-v1" || rollcall_id(4, BE) || initiator_addr(4, BE)
 *                         || responder_addr(4, BE)
 *
 * The context prefix domain-separates this from every other Ed25519 use in
 * the tree (the attestation self-signature uses "bramble-ident-v1", the
 * anchor endorsement "bramble-endorse-v1"). initiator_addr is inside the
 * signature so a response captured from one operator's roll-call cannot be
 * replayed into another operator's roll-call that happened to draw the same
 * id. The ROUND is deliberately NOT signed: a member answers a roll-call
 * once, and binding the round would make an otherwise valid answer look
 * forged after a re-announce.
 */
#define ROLLCALL_MSG_CONTEXT "bramble-rollcall-v1"
#define ROLLCALL_MSG_CONTEXT_LEN 19
#define ROLLCALL_MSG_SIZE (ROLLCALL_MSG_CONTEXT_LEN + 4 + 4 + 4) /* 31 */

typedef struct {
    uint32_t rollcall_id;
    uint8_t round;
    uint8_t text_len;
    char text[ROLLCALL_TEXT_MAX + 1]; /* NUL-terminated for the caller's convenience */
} rollcall_announce_t;

typedef struct {
    uint32_t rollcall_id;
    uint8_t round;
    uint32_t responder_addr;
    uint8_t sig[ROLLCALL_SIG_SIZE];
} rollcall_response_t;

/*
 * Encode/decode. Encoders return the number of bytes written, or 0 when the
 * output buffer is too small or an input is out of range (text_len over
 * ROLLCALL_TEXT_MAX, round outside 1..ROLLCALL_MAX_ROUNDS, rollcall_id 0).
 * Decoders return false and leave *out untouched on any malformed input,
 * including a length that does not match the frame's own declared shape.
 *
 * rollcall_id 0 is reserved as "no roll-call" and is rejected on both sides,
 * so a zeroed buffer can never decode into a plausible-looking roll-call.
 */
size_t rollcall_announce_encode(const rollcall_announce_t* in, uint8_t* out, size_t out_cap);
bool rollcall_announce_decode(const uint8_t* buf, size_t len, rollcall_announce_t* out);

size_t rollcall_response_encode(const rollcall_response_t* in, uint8_t* out, size_t out_cap);
bool rollcall_response_decode(const uint8_t* buf, size_t len, rollcall_response_t* out);

/*
 * Write the ROLLCALL_MSG_SIZE-byte canonical message the response signature
 * covers. Signer and verifier both call this, so the signed bytes cannot
 * diverge between them. Returns ROLLCALL_MSG_SIZE, or 0 if out_cap is too
 * small.
 */
size_t rollcall_signed_msg(uint32_t rollcall_id, uint32_t initiator_addr, uint32_t responder_addr,
                           uint8_t* out, size_t out_cap);

/* ── Response stagger ────────────────────────────────────────────────── */

/*
 * A roll-call asks every member to transmit at once, which is precisely the
 * N-node response storm that makes naive "ping everyone" primitives useless
 * on a shared half-duplex channel. Responses are therefore SLOTTED and
 * jittered before they ever reach the TX gate.
 *
 * The slot rule mirrors the broadcast delivery receipt's
 * (main/broadcast_delivery_receipt.c): a deterministic bucket drawn from the
 * responder's own address XOR the roll-call id, over a window sized at two
 * buckets per known peer and clamped to [4, 32]. Same rationale, same
 * shape: address-derived so two nodes rarely collide, id-derived so the SAME
 * two nodes do not collide again on the next roll-call, peer-count-sized so
 * a dense mesh spreads wider than a sparse one.
 *
 * The BASE and STEP are the roll-call's own rather than the receipt's, and
 * deliberately wider: a response frame is ~137 bytes on air where a receipt
 * is ~36, and the collection window it has to fit inside is a round
 * interval rather than a chat message's lifetime. Reusing the receipt's
 * 500ms step would pack the bigger frames tighter than the airtime they
 * occupy.
 *
 * random_value is caller-supplied (esp_random on device, the seeded PCG in
 * the simulator) so this function stays pure and reproducible under test.
 */
#define ROLLCALL_RESPONSE_BASE_MS 400u
#define ROLLCALL_RESPONSE_SLOT_MS 600u
#define ROLLCALL_RESPONSE_JITTER_MS 500u
#define ROLLCALL_SLOT_BUCKETS_MIN 4u
#define ROLLCALL_SLOT_BUCKETS_MAX 32u

uint32_t rollcall_response_slot(uint32_t responder_addr, uint32_t rollcall_id, uint8_t peer_count);
uint32_t rollcall_response_delay_ms(uint32_t responder_addr, uint32_t rollcall_id,
                                    uint8_t peer_count, uint32_t random_value);

/* ── Round schedule and the collection window ────────────────────────── */

/*
 * Round 1 goes out the moment the roll-call starts. Each further round waits
 * ROLLCALL_ROUND_BASE_MS doubled per round already sent (30s, then 60s), so
 * a node that missed the first flood gets two more chances while the total
 * cost stays bounded at ROLLCALL_MAX_ROUNDS floods. The backoff exists
 * because the second round is the one most likely to collide with the first
 * round's own response storm: it has to start after that storm has drained,
 * and the storm's own worst case is the slot window above (up to ~19s).
 *
 * After the LAST round the initiator keeps collecting for
 * ROLLCALL_COLLECT_TAIL_MS, then closes the ledger. A response that arrives
 * after the close is counted (rollcall_ledger_t.late) but does not reopen
 * the roll-call, so "closed" is a stable, reportable state.
 */
#define ROLLCALL_ROUND_BASE_MS 30000u
#define ROLLCALL_COLLECT_TAIL_MS 45000u

/* Delay from the round just sent to the next one. rounds_sent is 1-based
 * and must be >= 1; returns 0 once rounds_sent has reached
 * ROLLCALL_MAX_ROUNDS (no further round is due). */
uint32_t rollcall_round_delay_ms(uint8_t rounds_sent);

/* Total wall time from start to ledger close, for the operator-facing
 * countdown. Derived from the same constants as the schedule so the UI can
 * never show a window the firmware does not honor. */
uint32_t rollcall_window_ms(void);

/* ── Initiation rate limit ───────────────────────────────────────────── */

/*
 * A roll-call is the most expensive primitive in the protocol: one flood per
 * round plus one unicast response per member. Initiation is therefore
 * bounded, and the bound is enforced here rather than left to operator
 * discipline or to a UI that a script can bypass.
 *
 * ROLLCALL_MIN_INTERVAL_MS is the floor between two roll-calls STARTED by
 * this node. It is measured from start to start, so a roll-call that is
 * still collecting also blocks a new one (that case reports BUSY, which is
 * a different operator problem than "too soon" and so gets its own code).
 * The limiter is a plain last-start timestamp rather than a token bucket:
 * there is nothing to burst here, one at a time is the whole policy.
 */
#define ROLLCALL_MIN_INTERVAL_MS 300000u

typedef enum {
    ROLLCALL_START_OK = 0,
    ROLLCALL_START_BUSY,         /* a roll-call this node started is still open */
    ROLLCALL_START_RATE_LIMITED, /* within ROLLCALL_MIN_INTERVAL_MS of the last start */
} rollcall_start_check_t;

typedef struct {
    bool ever_started;
    uint32_t last_start_ms;
} rollcall_rate_state_t;

void rollcall_rate_init(rollcall_rate_state_t* rl);

/*
 * Non-mutating decision. ledger_open is the caller's answer to "is a
 * roll-call I started still collecting", kept as an explicit input so this
 * function does not need to see the ledger. now_ms is monotonic; the
 * comparison is wrap-safe (unsigned difference), matching every other
 * millisecond deadline in the tree.
 */
rollcall_start_check_t rollcall_rate_check(const rollcall_rate_state_t* rl, bool ledger_open,
                                           uint32_t now_ms);

/* Record an accepted start. Callers must only call this after
 * rollcall_rate_check returned ROLLCALL_START_OK. */
void rollcall_rate_note_start(rollcall_rate_state_t* rl, uint32_t now_ms);

/* ── Member side: answer-once bookkeeping ────────────────────────────── */

typedef struct {
    bool used;
    uint32_t rollcall_id;
    uint32_t initiator_addr;
    uint32_t seen_ms;
} rollcall_seen_entry_t;

typedef struct {
    rollcall_seen_entry_t entries[ROLLCALL_SEEN_MAX];
} rollcall_seen_table_t;

void rollcall_seen_init(rollcall_seen_table_t* t);

/*
 * Claim the right to answer (rollcall_id, initiator_addr) exactly once.
 * Returns true when this is the FIRST time this node has been asked and the
 * claim was recorded; false when it has already answered (or already queued
 * an answer) for this roll-call, which is what makes rounds 2 and 3
 * idempotent for a member that heard round 1.
 *
 * The pair is the key, not the id alone: two initiators can independently
 * draw the same 32-bit id, and collapsing them would silence a member for a
 * roll-call it never actually answered.
 *
 * When the table is full the OLDEST entry (by seen_ms) is recycled. That is
 * the right eviction for this table: the entries exist to suppress
 * re-answers within one roll-call's window, and the oldest entry is the one
 * whose window has most likely already closed.
 */
bool rollcall_seen_claim(rollcall_seen_table_t* t, uint32_t rollcall_id, uint32_t initiator_addr,
                         uint32_t now_ms);

/* Whether a claim is already recorded, without taking one. */
bool rollcall_seen_contains(const rollcall_seen_table_t* t, uint32_t rollcall_id,
                            uint32_t initiator_addr);

/*
 * Drop a claim, so a later announce round for the same (rollcall_id,
 * initiator_addr) is treated as never having been taken on.
 *
 * A claim is a promise to answer, and it must be released the moment the
 * promise cannot be kept: if a claimed answer never reaches the air (the
 * signature failed, the encode failed, or the budget-gated TX path refused
 * it) then the bounded re-announce rounds are the member's only remaining
 * chance to take part, and a claim left behind is exactly what silences it
 * for the rest of the roll-call. Callers therefore claim only once the
 * answer is queued, and release when a queued answer is abandoned.
 */
void rollcall_seen_release(rollcall_seen_table_t* t, uint32_t rollcall_id, uint32_t initiator_addr);

/* ── Member side: the answer budget ──────────────────────────────────── */

/*
 * How often this node is willing to ANSWER roll-calls, whoever asks.
 *
 * The initiation limit (ROLLCALL_MIN_INTERVAL_MS) binds only the node
 * running the script. It says nothing about what the rest of the fleet is
 * asked to spend, and a network-key insider holding the public channel can
 * broadcast a fresh announce as fast as it likes: every announce costs every
 * member an Ed25519 signature, a 137-byte unicast with its retry ladder and
 * a flooded delivery receipt. tx_gate would keep each node inside its own
 * airtime budget, but that is not protection, it is the damage: the budget
 * gets CONSUMED by roll-call answers and the node's chat, beacons and
 * receipts are denied by the lane the roll-call filled.
 *
 * So the member side carries its own bound. At most
 * ROLLCALL_ANSWER_MAX_PER_HOUR answers leave this node in any rolling
 * ROLLCALL_ANSWER_WINDOW_MS, which is what makes the primitive's fleet-wide
 * cost bounded by construction rather than by the initiator's good manners.
 * The value is two initiators running continuously at their own
 * ROLLCALL_MIN_INTERVAL_MS floor, so a cooperative fleet never meets it.
 *
 * A refused answer takes NO claim (rollcall_seen_claim), so a later round of
 * the same roll-call is answered if the budget has room by then; refusals
 * are counted and reported rather than silently swallowed.
 */
#define ROLLCALL_ANSWER_WINDOW_MS 3600000u
#define ROLLCALL_ANSWER_MAX_PER_HOUR 12u

typedef struct {
    bool used[ROLLCALL_ANSWER_MAX_PER_HOUR];
    uint32_t at_ms[ROLLCALL_ANSWER_MAX_PER_HOUR];
} rollcall_answer_budget_t;

void rollcall_answer_budget_init(rollcall_answer_budget_t* b);

/* Answers still inside the window at now_ms. */
uint8_t rollcall_answer_budget_used(const rollcall_answer_budget_t* b, uint32_t now_ms);

/* Whether one more answer fits. Non-mutating; wrap-safe like every other
 * millisecond deadline in the tree. */
bool rollcall_answer_budget_allow(const rollcall_answer_budget_t* b, uint32_t now_ms);

/* Charge one answer. Callers must only call this after
 * rollcall_answer_budget_allow returned true. */
void rollcall_answer_budget_note(rollcall_answer_budget_t* b, uint32_t now_ms);

/* ── Initiator side: the ledger ──────────────────────────────────────── */

typedef struct {
    bool used;
    uint32_t addr;
    /* An ATTESTED response arrived: the Ed25519 signature over the canonical
     * message verified against this address's pinned identity key. Rows
     * without it exist only when the delivery-receipt machinery reported a
     * path for a node that never answered. */
    bool responded;
    uint32_t responded_at_ms; /* monotonic ms, valid only when responded */
    uint8_t round;            /* the announce round the response named */
    /* Relay path as reported by the existing broadcast delivery-receipt
     * machinery, normalized initiator -> ... -> responder. have_path is
     * false when no receipt arrived, which is the common case on a mesh
     * whose receipt policy has stood down (see docs/rollcall.md). */
    bool have_path;
    uint8_t hop_count;
    uint32_t relay_path[ROLLCALL_PATH_MAX];
} rollcall_entry_t;

typedef struct {
    /* open: the roll-call is still collecting. A ledger stays readable after
     * it closes; `open` is what stops it changing. */
    bool active;
    bool open;
    uint32_t rollcall_id;
    uint32_t initiator_addr;
    uint32_t started_ms;
    uint32_t closed_ms; /* valid only when active && !open */
    uint8_t rounds_sent;
    uint32_t next_round_ms;
    uint8_t text_len;
    char text[ROLLCALL_TEXT_MAX + 1];

    /* The expected set. anchored is TRUE only when this node pins
     * anchor-certified peers, which is the only configuration in which an
     * expected set is authoritative; see docs/rollcall.md. On an un-anchored
     * mesh expected_count is 0 and the ledger reports observed responders
     * only. */
    bool anchored;
    uint8_t expected_count;
    uint32_t expected[ROLLCALL_MAX_EXPECTED];

    uint8_t entry_count;
    rollcall_entry_t entries[ROLLCALL_MAX_RESPONDERS];

    /* Honest accounting for everything the ledger could not record.
     *   overflow    responses dropped because the table was full
     *   unattested  responses whose signature did not verify, or whose
     *               responder holds no pinned identity key here (an
     *               unpinned peer cannot be attested, only observed)
     *   late        attested responses that arrived after the close */
    uint32_t overflow;
    uint32_t unattested;
    uint32_t late;
} rollcall_ledger_t;

void rollcall_ledger_init(rollcall_ledger_t* l);

/*
 * Open a ledger. text/text_len is the operator payload (text may be NULL
 * when text_len is 0); expected/expected_count is the anchor-certified peer
 * set, which the caller passes in rather than this file reading an identity
 * store, so the ledger stays pure. Pass expected_count 0 and anchored false
 * on an un-anchored mesh.
 *
 * Returns false (leaving the ledger untouched) for rollcall_id 0 or a
 * text_len over ROLLCALL_TEXT_MAX. Any previous ledger is replaced: a node
 * tracks one roll-call of its own at a time, which is what the BUSY arm of
 * the rate check enforces at the layer above.
 */
bool rollcall_ledger_start(rollcall_ledger_t* l, uint32_t rollcall_id, uint32_t initiator_addr,
                           uint32_t now_ms, const char* text, uint8_t text_len,
                           const uint32_t* expected, uint8_t expected_count, bool anchored);

/* Record that round `round` went out at now_ms and schedule the next one.
 * Returns the absolute ms at which the next round is due, or 0 when the last
 * round has now been sent. */
uint32_t rollcall_ledger_note_round(rollcall_ledger_t* l, uint8_t round, uint32_t now_ms);

/* Whether another announce round is due at now_ms. */
bool rollcall_ledger_round_due(const rollcall_ledger_t* l, uint32_t now_ms);

/* Absolute ms at which the ledger should close. */
uint32_t rollcall_ledger_close_at(const rollcall_ledger_t* l);

/* Close the ledger if its window has elapsed. Returns true iff this call
 * closed it (so the caller emits exactly one completion notification). */
bool rollcall_ledger_maybe_close(rollcall_ledger_t* l, uint32_t now_ms);

/*
 * Record an ATTESTED response. The caller has already verified the Ed25519
 * signature; this function records the fact, not the proof. Idempotent by
 * responder address: a duplicate keeps the FIRST timestamp and round, so a
 * retransmitted response cannot make a member look later than it answered.
 *
 * Returns true when the response was recorded (including the idempotent
 * repeat), false when it was rejected: a closed ledger (counted as `late`),
 * a mismatched roll-call id, or a full table (counted as `overflow`).
 *
 * An answer outranks a path. When the table is full, a row that carries only
 * a relay path (have_path, never responded) is reclaimed for the answer, so
 * `overflow` can only ever mean "more members answered than this ledger can
 * hold", never "receipts got here first".
 */
bool rollcall_ledger_note_response(rollcall_ledger_t* l, uint32_t rollcall_id,
                                   uint32_t responder_addr, uint8_t round, uint32_t now_ms);

/* Count a response that could not be attested (bad signature, or no pinned
 * key for the responder). Recorded as a number, never as a ledger row: an
 * unattested response is not evidence that anybody answered. */
void rollcall_ledger_note_unattested(rollcall_ledger_t* l);

/*
 * Attach the relay path the delivery-receipt machinery reported for a
 * responder. hop_count is clamped to ROLLCALL_PATH_MAX.
 *
 * responder_pinned is the caller's answer to "do I hold a verified identity
 * pin for this address", and it gates ROW CREATION. A delivery receipt is
 * authenticated by the shared network key alone, and its source address is
 * chosen by whoever built it, so a receipt is not evidence that the named
 * node exists. Without the gate, one insider that read an announce's
 * cleartext packet_id off the air could mint ROLLCALL_MAX_RESPONDERS
 * receipts under invented addresses, fill the table before the first
 * staggered answer was even due, and turn every real member of the fleet
 * into a `missing` row. Restricted to pinned addresses, a forged receipt can
 * only occupy a slot the named member was already entitled to, and an
 * arriving answer reclaims it anyway (rollcall_ledger_note_response).
 *
 * A path for an unpinned address attaches to an existing row and creates
 * none, which loses nothing the ledger could have reported: an answer from
 * an address this node holds no pin for is `unattested` and never becomes a
 * row either.
 *
 * Returns false when the ledger is closed, the id does not match, the table
 * is full, or no row exists and none may be created.
 */
bool rollcall_ledger_note_path(rollcall_ledger_t* l, uint32_t rollcall_id, uint32_t responder_addr,
                               bool responder_pinned, uint8_t hop_count,
                               const uint32_t* relay_path);

/* Look up a row by address, or NULL. */
const rollcall_entry_t* rollcall_ledger_find(const rollcall_ledger_t* l, uint32_t addr);

/* Number of rows with an attested response. */
uint8_t rollcall_ledger_responded_count(const rollcall_ledger_t* l);

/*
 * Expected addresses with no attested response, written to out (up to
 * out_cap). Returns the TOTAL missing count, which may exceed the number
 * written, so a caller that truncates still reports the true total.
 *
 * On an un-anchored ledger (anchored false, expected_count 0) this is always
 * 0: there is no authoritative expected set, so nothing can be called
 * missing. That is a deliberate honesty bound, not an oversight; see
 * docs/rollcall.md.
 */
uint8_t rollcall_ledger_missing(const rollcall_ledger_t* l, uint32_t* out, uint8_t out_cap);

#endif /* BRAMBLE_ROLLCALL_H */
