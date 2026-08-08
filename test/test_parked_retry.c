/* Parked-message retry trigger (main/parked_retry.c).
 *
 * The rejoin edge alone (a beacon ADMITTING an address to the neighbor table)
 * cannot reach the common case: a peer whose ACKs are being lost keeps
 * beaconing, so it never leaves the table and can never newly join it, and a
 * message parked for it would sit QUEUED forever. These tests drive the real
 * decision kernel, the real neighbor table (components/routing) and the real
 * message store; only the transmit half is faked, since mesh_task.c and
 * mesh_beacon.c are ESP-IDF-only and never host-compiled.
 */
#include "unity.h"

#include <string.h>

#include "msg_store.h"
#include "parked_retry.h"
#include "routing.h"

static neighbor_table_t s_nb;
static parked_sweep_t s_sweep;
static int s_store_scans;          /* flush attempts, i.e. scans of the message store */
static bool s_resend_succeeds;     /* whether the faked transmit lands an ACK */
static uint32_t s_resend_fail_uid; /* one uid the faked transmit refuses, 0 for none */

/* The send queue, mirrored from mesh_task.c's queue_message and mesh_dm.c's
 * queue_session_message: a fixed slot array, first free slot wins, entries
 * drained when the route or session they were waiting for arrives.
 *
 * Modelling it is not decoration. A re-send that cannot go out right now does
 * not fail, it QUEUES, and the row stays parked while it sits there. A harness
 * that just writes a status hides the whole failure mode this queue creates:
 * the same row occupying two slots and going out twice, which is one message
 * written by the user and two delivered to the peer. The delivery counter
 * below is what makes that visible. */
#define FAKE_QUEUE_SLOTS 8
#define FAKE_MAX_UID 32
static struct {
    bool used;
    uint32_t uid;
} s_queue[FAKE_QUEUE_SLOTS];
static int s_delivered[FAKE_MAX_UID]; /* times each uid actually reached the peer */

/* The third outcome, and the one the other two cannot express: the frame goes
 * out and is never acknowledged. It is neither a success nor a synchronous
 * failure, it is a transmit whose verdict arrives seconds later from the ACK
 * retry tick, and on a marginal link it is the NORMAL outcome. */
static bool s_resend_transmits_but_no_ack;

/* The same transmit, but left OUTSTANDING: the frame is on the air and neither
 * an ACK nor a give-up has arrived yet. */
static bool s_resend_stays_in_flight;

/* Mirrors the one thing the flush needs from the pending-ack table: whether
 * this row's last frame is still awaiting acknowledgement. In the firmware
 * that is pending_ack_is_active(&s_pending_acks, row.packet_id). */
static uint32_t s_in_flight_uid;

/* Frames actually put on the air per uid. This is the measure that matters for
 * duplication: two frames for one uid carry two different packet_ids, and the
 * receiver dedups on packet_id, so the peer displays one written message
 * twice. */
static int s_frames_on_air[FAKE_MAX_UID];

void setUp(void) {
    neighbor_init(&s_nb);
    memset(&s_sweep, 0, sizeof(s_sweep));
    memset(s_queue, 0, sizeof(s_queue));
    memset(s_delivered, 0, sizeof(s_delivered));
    memset(s_frames_on_air, 0, sizeof(s_frames_on_air));
    msg_store_init();
    s_store_scans = 0;
    s_resend_succeeds = true;
    s_resend_fail_uid = 0;
    s_resend_transmits_but_no_ack = false;
    s_resend_stays_in_flight = false;
    s_in_flight_uid = 0;
}

void tearDown(void) {}

static void deliver(uint32_t uid) {
    TEST_ASSERT_LESS_THAN_UINT32(FAKE_MAX_UID, uid);
    s_delivered[uid]++;
    msg_store_update_by_uid(uid, 0x1000u + uid, MSG_STATUS_DELIVERED);
}

static int delivered_count(uint32_t uid) {
    TEST_ASSERT_LESS_THAN_UINT32(FAKE_MAX_UID, uid);
    return s_delivered[uid];
}

/* Mirrors one re-send out of mesh_flush_parked_for. Either it goes out now, or
 * there is no route or session yet and it is queued for one; a queued row
 * stays QUEUED, which is what lets a later flush pick it up again. */
/* Mirrors the transmit-then-silence path, using the REAL msg_store calls the
 * firmware makes and in the same order: mesh_send_dm stamps the row with its
 * wire packet_id and marks it SENT (mesh_dm.c, the active-session branch), and
 * seconds later the ACK retry tick gives up and reports FAILED against that
 * packet_id (mesh_task.c's pending-ack tick). Whether the row is still parked
 * afterwards is decided entirely by msg_store's transition rules, which is
 * what this drives. */
static void transmit_without_ack(uint32_t uid) {
    uint32_t pkt = 0x9000u + uid;
    msg_store_update_by_uid(uid, pkt, MSG_STATUS_SENT);
    msg_store_update_status(pkt, MSG_STATUS_FAILED);
}

static void fake_resend(uint32_t uid) {
    TEST_ASSERT_LESS_THAN_UINT32(FAKE_MAX_UID, uid);
    if (s_resend_stays_in_flight) {
        /* The frame is on the air and the verdict has not arrived. The row is
         * stamped exactly as mesh_send_dm stamps it. */
        s_frames_on_air[uid]++;
        msg_store_update_by_uid(uid, 0x9000u + uid, MSG_STATUS_SENT);
        s_in_flight_uid = uid;
        return;
    }
    if (s_resend_transmits_but_no_ack) {
        s_frames_on_air[uid]++;
        transmit_without_ack(uid);
        return;
    }
    if (s_resend_succeeds && uid != s_resend_fail_uid) {
        s_frames_on_air[uid]++;
        deliver(uid);
        return;
    }
    /* One entry per uid, mirroring the same rule in queue_message and
     * queue_session_message: a row already waiting to go out is not queued
     * again, whatever asked for it a second time. */
    for (int i = 0; i < FAKE_QUEUE_SLOTS; i++) {
        if (s_queue[i].used && s_queue[i].uid == uid)
            return;
    }
    for (int i = 0; i < FAKE_QUEUE_SLOTS; i++) {
        if (!s_queue[i].used) {
            s_queue[i].used = true;
            s_queue[i].uid = uid;
            return;
        }
    }
    /* Queue full: the send fails outright, and msg_store's sticky rule keeps
     * the row parked rather than letting it go FAILED. */
    msg_store_update_by_uid(uid, 0, MSG_STATUS_FAILED);
}

/* The route or session the queue was waiting for arrives, so everything in it
 * goes out (flush_queued_messages / flush_session_queue). */
static void send_queue_drains(void) {
    for (int i = 0; i < FAKE_QUEUE_SLOTS; i++) {
        if (s_queue[i].used) {
            s_queue[i].used = false;
            s_frames_on_air[s_queue[i].uid]++;
            deliver(s_queue[i].uid);
        }
    }
}

static int frames_on_air(uint32_t uid) {
    TEST_ASSERT_LESS_THAN_UINT32(FAKE_MAX_UID, uid);
    return s_frames_on_air[uid];
}

/* Mirrors mesh_flush_parked_for (mesh_task.c): select the peer's parked rows,
 * re-send each, and report how many rows it found. */
static int fake_flush_parked_for(uint32_t peer_addr) {
    uint32_t uids[MSG_STORE_MAX];
    s_store_scans++;
    int n = msg_store_parked_uids_for_peer(peer_addr, uids, MSG_STORE_MAX);
    for (int i = 0; i < n; i++) {
        /* A row whose last frame is still awaiting an ACK is skipped, not
         * re-sent: the send queue's uid keying cannot cover this one, because a
         * transmitted frame holds no queue entry. It is still COUNTED, so the
         * peer stays armed and the row gets its next attempt once the frame has
         * resolved. Mirrors the pending_ack_is_active guard in
         * mesh_flush_parked_for. */
        if (uids[i] == s_in_flight_uid)
            continue;
        fake_resend(uids[i]);
    }
    return n;
}

/* Mirrors handle_beacon (mesh_beacon.c): update the neighbor table, then run
 * the parked-flush block against the resulting is_new_peer. Returns the number
 * of parked rows the flush found, or -1 when the beacon did not flush at all. */
static int beacon_from(uint32_t peer_addr, uint32_t now_ms) {
    int idx = neighbor_update(&s_nb, peer_addr, -60, 8, 0xABCDu, now_ms);
    bool is_new_peer = neighbor_is_newly_admitted(&s_nb, idx, now_ms);
    if (!parked_retry_beacon_decide_flush(&s_nb, &s_sweep, peer_addr, is_new_peer, now_ms))
        return -1;
    int found = fake_flush_parked_for(peer_addr);
    parked_retry_flushed(&s_nb, peer_addr, found, now_ms);
    return found;
}

/* Mirrors the parked sweep in mesh_periodic_maintenance (mesh_task.c): once
 * per sweep interval, take the next peer with parked messages and try it,
 * unless it is a neighbor with a live trigger of its own. Returns the peer it
 * attempted, or 0. */
static uint32_t sweep_at(uint32_t now_ms) {
    if (!parked_retry_sweep_due(&s_sweep, now_ms))
        return 0;
    uint32_t peer = 0;
    if (!msg_store_next_parked_peer(s_sweep.last_peer, &peer))
        return 0;
    if (parked_retry_sweep_defers_to_beacon(&s_nb, peer)) {
        parked_retry_sweep_skipped(&s_sweep, peer);
        return 0;
    }
    parked_retry_swept(&s_sweep, peer, now_ms);
    fake_flush_parked_for(peer);
    return peer;
}

/* An hour of ordinary life: one in-range peer beaconing on cadence, and the
 * maintenance tick. The tick is really the mesh task's ~10ms loop
 * (mesh_task.c, vTaskDelay(pdMS_TO_TICKS(10))), so anything paced here is
 * paced by its own schedule and not by how often it is asked; 60s steps just
 * keep these loops short, and a 10ms step gives the same answers. */
static void run_an_hour_from(uint32_t start_ms, uint32_t chatty_peer) {
    for (uint32_t t = start_ms; t <= start_ms + 3600000u; t += 60000u) {
        if (chatty_peer != 0)
            beacon_from(chatty_peer, t);
        sweep_at(t);
    }
}

/* Mirrors mesh_park_message (mesh_task.c): move a failed outgoing DM to
 * QUEUED, then arm its peer. Returns the row's uid. */
static uint32_t park_dm_for(uint32_t peer_addr, const char* text, uint32_t now_ms) {
    uint32_t uid = msg_store_next_uid();
    msg_store_add_dm_uid(peer_addr, MSG_DIR_OUTGOING, text, strlen(text), 0, 0, 0,
                         MSG_STATUS_FAILED, uid);
    TEST_ASSERT_TRUE(msg_store_update_by_uid(uid, 0, MSG_STATUS_QUEUED));
    parked_retry_arm(&s_nb, peer_addr, now_ms);
    return uid;
}

static msg_status_t status_of(uint32_t uid) {
    stored_msg_t m;
    TEST_ASSERT_TRUE(msg_store_get_copy_by_uid(uid, &m));
    return m.status;
}

/* 1. The defect. */
void test_parked_message_is_delivered_when_the_peer_never_left(void) {
    const uint32_t peer = 0xAA110011u;

    /* The peer is here and stays here: it beacons on cadence throughout, so
     * the rejoin edge fires once, now, and can never fire again. */
    TEST_ASSERT_EQUAL_INT(0, beacon_from(peer, 1000));

    uint32_t uid = park_dm_for(peer, "are you still there", 5000);
    TEST_ASSERT_EQUAL(MSG_STATUS_QUEUED, status_of(uid));

    beacon_from(peer, 65000);

    TEST_ASSERT_EQUAL_MESSAGE(MSG_STATUS_DELIVERED, status_of(uid),
                              "parked DM never went out: the peer beaconed but, having never "
                              "left the neighbor table, never rejoined it either");
}

/* 2. The cooldown. */
void test_a_stuck_peer_gets_one_attempt_per_cooldown(void) {
    const uint32_t peer = 0xBB220022u;
    s_resend_succeeds = false; /* present, but nothing gets through */

    beacon_from(peer, 1000);
    uint32_t uid = park_dm_for(peer, "still stuck", 2000);
    int before = s_store_scans;

    /* Twenty beacons in the following minute. */
    for (uint32_t t = 3000; t <= 63000; t += 3000)
        beacon_from(peer, t);

    TEST_ASSERT_EQUAL_INT_MESSAGE(before + 1, s_store_scans,
                                  "a stuck peer must not draw a flush attempt per beacon");
    TEST_ASSERT_EQUAL(MSG_STATUS_QUEUED, status_of(uid));

    /* Still not due one millisecond early, due one millisecond later. */
    beacon_from(peer, 3000 + PARKED_RETRY_COOLDOWN_MS - 1);
    TEST_ASSERT_EQUAL_INT(before + 1, s_store_scans);
    beacon_from(peer, 3000 + PARKED_RETRY_COOLDOWN_MS);
    TEST_ASSERT_EQUAL_INT(before + 2, s_store_scans);
}

/* 3a. The rejoin edge, for a peer that was never seen at all. */
void test_rejoin_edge_still_flushes_a_peer_that_was_never_in_the_table(void) {
    const uint32_t peer = 0xCC330033u;

    /* Nothing to arm: the peer has no entry. That is the case the rejoin edge
     * has always covered, and it must keep covering it. */
    uint32_t uid = park_dm_for(peer, "when you show up", 2000);
    TEST_ASSERT_FALSE(parked_retry_arm(&s_nb, peer, 2000));

    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 900000));
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, status_of(uid));
}

/* 3b. The rejoin edge, for a peer that really left and came back. */
void test_rejoin_edge_still_flushes_a_peer_that_left_and_returned(void) {
    const uint32_t peer = 0xDD440044u;

    beacon_from(peer, 1000);
    uint32_t uid = park_dm_for(peer, "see you later", 2000);

    /* It goes quiet and ages out. */
    neighbor_purge(&s_nb, 1000 + NEIGHBOR_EXPIRY_MS);
    TEST_ASSERT_EQUAL_INT(0, neighbor_count(&s_nb));

    /* Coming back re-admits it, which is the rejoin edge, on a fresh entry
     * that carries no arming from before it left. */
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 1000 + NEIGHBOR_EXPIRY_MS + 1));
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, status_of(uid));
}

/* 3c. The rejoin edge gets ONE shot, and the moment it fires is the moment
 * most likely to fail: a peer that has just come back into range is on a
 * marginal link, and everything else that just became reachable is competing
 * for the airtime budget. A rejoin flush that fails must not strand the
 * message, which it would if the rejoin edge were the only trigger and the
 * peer then stayed in the table for good. */
void test_a_failed_rejoin_flush_still_leaves_a_trigger(void) {
    const uint32_t peer = 0x66AA00BBu;
    s_resend_succeeds = false;

    /* Parked while the peer is absent, so there is no entry to arm: the rejoin
     * edge is the only thing that can deliver this. */
    uint32_t uid = park_dm_for(peer, "sent while you were out", 2000);
    TEST_ASSERT_EQUAL_INT(0, neighbor_count(&s_nb));

    /* It comes back, the rejoin edge fires, and the re-send fails. */
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 100000));
    TEST_ASSERT_EQUAL(MSG_STATUS_QUEUED, status_of(uid));

    /* It now stays in the table, so no second rejoin edge is possible ever
     * again. The message must still have a future. */
    s_resend_succeeds = true;
    beacon_from(peer, 100000 + PARKED_RETRY_COOLDOWN_MS);

    TEST_ASSERT_EQUAL_MESSAGE(MSG_STATUS_DELIVERED, status_of(uid),
                              "the rejoin edge fired once, missed, and stranded the message: a "
                              "failed flush must leave the peer armed for another attempt");
}

/* 3d. Partially successful flush: whatever is left behind keeps a trigger. */
void test_a_partial_flush_keeps_a_trigger_for_the_row_left_behind(void) {
    const uint32_t peer = 0x77BB00CCu;

    beacon_from(peer, 1000);
    uint32_t went = park_dm_for(peer, "this one goes", 2000);
    uint32_t stuck = park_dm_for(peer, "this one does not", 2100);
    s_resend_fail_uid = stuck;

    TEST_ASSERT_EQUAL_INT(2, beacon_from(peer, 3000));
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, status_of(went));
    TEST_ASSERT_EQUAL(MSG_STATUS_QUEUED, status_of(stuck));

    /* The half that failed is still parked, so the peer stays armed and the
     * next attempt picks up that row alone. */
    s_resend_fail_uid = 0;
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 3000 + PARKED_RETRY_COOLDOWN_MS));
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, status_of(stuck));
}

/* 3e. A full neighbor table admits a peer by EVICTING one, so the table's
 * count does not change and "the count grew" stops recognising a new peer at
 * all. On a mesh with MAX_NEIGHBORS neighbors that silently removes the rejoin
 * edge, which is the only trigger a message parked for an absent peer has. */
void test_a_peer_readmitted_into_a_full_table_still_flushes(void) {
    const uint32_t peer = 0x88CC00DDu;

    /* Fill the table. Distinct addresses, ascending times, so the first one is
     * the eviction victim. */
    for (int i = 0; i < MAX_NEIGHBORS; i++)
        beacon_from(0x1000u + (uint32_t)i, 1000 + (uint32_t)i * 10);
    TEST_ASSERT_EQUAL_INT(MAX_NEIGHBORS, neighbor_count(&s_nb));

    uint32_t uid = park_dm_for(peer, "no room at the inn", 5000);
    TEST_ASSERT_FALSE(parked_retry_arm(&s_nb, peer, 5000)); /* absent: nothing to arm */

    /* It arrives and takes the oldest entry's slot. The count is unchanged. */
    int found = beacon_from(peer, 10000);
    TEST_ASSERT_EQUAL_INT(MAX_NEIGHBORS, neighbor_count(&s_nb));

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, found,
                                  "a peer admitted into a full table is a new peer, but the "
                                  "table count cannot grow to say so");
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, status_of(uid));
}

/* 3f. The peer that is not a neighbour at all. A DM to a peer two hops away
 * is an ordinary send that can fail and be parked like any other, and the user
 * cannot tell one hop from two, so the promise is the same. But that peer
 * sends this node no beacons, so no beacon-driven trigger can ever fire for
 * it: neither the rejoin edge nor an armed entry, because it has no entry to
 * arm. Only the sweep reaches it. */
void test_a_parked_message_reaches_a_peer_that_is_not_a_neighbour(void) {
    const uint32_t multi_hop = 0x99DD00EEu;
    const uint32_t chatty = 0x1111FFFFu;

    beacon_from(chatty, 1000);
    uint32_t uid = park_dm_for(multi_hop, "two hops away", 2000);
    TEST_ASSERT_FALSE(parked_retry_arm(&s_nb, multi_hop, 2000)); /* no entry to arm */

    run_an_hour_from(60000, chatty);

    TEST_ASSERT_EQUAL_MESSAGE(MSG_STATUS_DELIVERED, status_of(uid),
                              "a peer reachable only over a route sends no beacons, so nothing "
                              "ever retried it: an hour of parked message, undelivered");
}

/* 3g. The sweep obeys the same spacing as everything else. */
void test_the_sweep_attempts_a_stuck_peer_once_per_sweep_interval(void) {
    const uint32_t multi_hop = 0x99DD00EEu;
    s_resend_succeeds = false;

    park_dm_for(multi_hop, "still trying", 1000);
    int before = s_store_scans;

    run_an_hour_from(60000, 0);

    /* One attempt at the first opportunity, then one per interval, and never
     * one per tick. This loop asks 61 times an hour; the firmware asks every
     * 10ms, so on a device the difference between paced and unpaced is 13
     * attempts against roughly 360000. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(1 + 3600000u / PARKED_RETRY_SWEEP_MS, s_store_scans - before,
                                  "the sweep must pace itself the same way the beacon path does");
}

/* 3h. The two triggers must not both attempt the same peer inside the window
 * where the send queue still holds the first attempt: the queue takes the
 * first free slot without keying on uid, so that would put one written message
 * on the air twice. */
void test_a_peer_the_sweep_just_tried_is_not_immediately_retried_by_its_beacon(void) {
    const uint32_t peer = 0xAB00CD00u;
    s_resend_succeeds = false;

    park_dm_for(peer, "hello?", 1000);
    TEST_ASSERT_EQUAL_HEX32(peer, sweep_at(2000)); /* not a neighbour yet: swept */
    int after_sweep = s_store_scans;

    /* It turns up as a direct neighbour moments later, which is the rejoin
     * edge and would otherwise flush unconditionally. */
    TEST_ASSERT_EQUAL_INT(-1, beacon_from(peer, 3000));
    TEST_ASSERT_EQUAL_INT(after_sweep, s_store_scans);

    /* The held edge is deferred, not lost: the peer joined while held, so
     * nothing else would ever have flushed it (the sweep passes over
     * neighbours), and it must go out when the hold ends. */
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 2000 + PARKED_RETRY_COOLDOWN_MS));
}

/* 3i. The sweep leaves direct neighbours to the beacon trigger, so the two
 * cannot both be counting down against the same peer. */
void test_the_sweep_passes_over_a_direct_neighbour_and_moves_on(void) {
    const uint32_t neighbour = 0x00000100u;
    const uint32_t multi_hop = 0x00000200u;
    s_resend_succeeds = false;

    beacon_from(neighbour, 1000);
    park_dm_for(neighbour, "you are close", 2000);
    park_dm_for(multi_hop, "you are far", 2100);

    /* Lowest address first, and it is a neighbour, so the sweep passes over
     * it without attempting anything. */
    TEST_ASSERT_EQUAL_HEX32(0, sweep_at(3000));
    /* The rotation still advanced, so the next sweep reaches the far peer. */
    TEST_ASSERT_EQUAL_HEX32(multi_hop, sweep_at(3000 + PARKED_RETRY_SWEEP_MS));
    /* And the neighbour's own trigger was never held off by that pass: its
     * one parked row still goes out on its own beacon. */
    TEST_ASSERT_EQUAL_INT(1, beacon_from(neighbour, 3000 + PARKED_RETRY_SWEEP_MS + 1000));
}

/* 3i-bis. The pass-over has one exception, and it is the difference between a
 * lost arm costing a delay and costing the message. A neighbour holding parked
 * rows with NOTHING armed has no trigger at all: the beacon path only flushes
 * an armed peer or a newly admitted one, and a peer that keeps beaconing is
 * never admitted again. Reachable in the ordinary course by a full table
 * evicting and readmitting the entry, which brings it back zeroed. */
void test_the_sweep_rescues_a_neighbour_whose_arming_was_lost(void) {
    const uint32_t peer = 0x0000AA00u;

    beacon_from(peer, 1000);
    uint32_t uid = park_dm_for(peer, "armed, then not", 2000);

    /* The arming is lost while the peer stays a neighbour and keeps beaconing:
     * no rejoin edge is possible and nothing is armed, so every beacon from
     * here on declines to flush. */
    neighbor_entry_t* e = neighbor_lookup(&s_nb, peer);
    TEST_ASSERT_NOT_NULL(e);
    e->parked_retry_after_ms = 0;
    TEST_ASSERT_EQUAL_INT(-1, beacon_from(peer, 3000));

    run_an_hour_from(60000, peer);

    TEST_ASSERT_EQUAL_MESSAGE(MSG_STATUS_DELIVERED, status_of(uid),
                              "a neighbour with parked rows and no arming has no trigger at "
                              "all, and the sweep passed over it because it is a neighbour");
}

/* 3i-ter. The hold belongs to the peer that was attempted, and only to it.
 * Asserted directly against the two calls rather than through a paced sweep,
 * because today the sweep interval and the cooldown are equal, so a stale hold
 * has always expired by the time the next sweep runs and an integrated test
 * would pass whether or not the hold is cleared. This is the contract that
 * keeps it that way if those two are ever tuned apart. */
void test_a_peer_the_sweep_passed_over_is_not_held_by_the_previous_peers_hold(void) {
    const uint32_t attempted = 0x0000BB00u;
    const uint32_t passed_over = 0x0000BB01u;

    parked_retry_swept(&s_sweep, attempted, 1000);
    parked_retry_sweep_skipped(&s_sweep, passed_over);

    /* The peer that was passed over has its own parked row and its own arming,
     * and its beacon must flush: the sweep never touched it. */
    beacon_from(passed_over, 1000);
    park_dm_for(passed_over, "mine to send", 1000);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, beacon_from(passed_over, 1000),
                                  "a peer the sweep passed over was silenced by a hold that "
                                  "belonged to the peer the sweep actually tried");
}

/* 3i-quater. The attempt that transmits and is never acknowledged. This is not
 * an edge case: it is the ordinary outcome on a marginal link, and a marginal
 * link is the reason the message was parked in the first place. The frame goes
 * out, the row is stamped SENT, and seconds later the ACK retry tick reports
 * FAILED against that packet_id. If SENT is allowed to happen, the row is no
 * longer QUEUED when the failure lands, the sticky rule that protects a parked
 * row does not apply, and the row goes FAILED. Nothing anywhere re-parks a row
 * (mesh_park_message is the only producer of QUEUED in the tree), so the
 * message is stranded after exactly one attempt, under a promise that it will
 * keep trying. */
void test_an_unacknowledged_attempt_leaves_the_message_parked(void) {
    const uint32_t peer = 0x0000CC00u;
    s_resend_transmits_but_no_ack = true;

    beacon_from(peer, 1000);
    uint32_t uid = park_dm_for(peer, "did you get this", 2000);
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 3000)); /* transmitted, never acknowledged */

    TEST_ASSERT_EQUAL_MESSAGE(MSG_STATUS_QUEUED, status_of(uid),
                              "an attempt that reached the air and was never acknowledged "
                              "un-parked the message: it is FAILED, nothing re-parks it, and it "
                              "will never be retried again");

    /* Still parked is only half of it. The point is that it gets another go. */
    s_resend_transmits_but_no_ack = false;
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 3000 + PARKED_RETRY_COOLDOWN_MS));
    TEST_ASSERT_EQUAL_MESSAGE(MSG_STATUS_DELIVERED, status_of(uid),
                              "the message stayed parked but never drew a second attempt");
}

/* 3i-quinquies. The flip side, and the hazard the fix above could create: a row
 * that stays QUEUED while its frame is still in the air is exactly the state
 * the next trigger could re-select, which would put one written message on the
 * air twice. The send queue's uid keying does not cover this, because a
 * transmitted frame holds no queue entry: it holds a pending ACK. So the flush
 * skips a row whose packet_id is still awaiting one.
 *
 * Deliberately NOT left to the cooldown outlasting the ACK retry budget (about
 * 14s at MSG_TIER_NORMAL against a 300s cooldown). That is true today and it is
 * a timing coincidence, which is the exact shape of the invariant this branch
 * already had to delete a set of assertions for. */
void test_a_flush_does_not_re_send_a_row_still_awaiting_an_ack(void) {
    const uint32_t peer = 0x0000DD00u;

    beacon_from(peer, 1000);
    uint32_t uid = park_dm_for(peer, "in the air", 2000);

    /* The attempt transmits and stays outstanding: no ACK, no give-up yet. */
    s_resend_stays_in_flight = true;
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 3000));
    TEST_ASSERT_EQUAL(MSG_STATUS_QUEUED, status_of(uid));
    TEST_ASSERT_EQUAL_INT(1, frames_on_air(uid));

    /* A second trigger reaches the same peer while that frame is outstanding.
     * It must find the row and leave it alone rather than send it again. */
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 3000 + PARKED_RETRY_COOLDOWN_MS));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, frames_on_air(uid),
                                  "a parked row was re-sent while its previous frame was still "
                                  "awaiting an ACK, so one written message went on the air twice "
                                  "under two packet_ids and the peer displays both");

    /* Once the frame resolves the row is free to be attempted again. */
    s_in_flight_uid = 0;
    s_resend_stays_in_flight = false;
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 3000 + 2 * PARKED_RETRY_COOLDOWN_MS));
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, status_of(uid));
    TEST_ASSERT_EQUAL_INT(2, frames_on_air(uid));
}

/* 3j. Rotation: every parked peer gets a turn, none starves. */
void test_the_sweep_gives_each_parked_peer_a_turn(void) {
    s_resend_succeeds = false;
    park_dm_for(0x300u, "c", 1000);
    park_dm_for(0x100u, "a", 1100);
    park_dm_for(0x200u, "b", 1200);

    uint32_t t = 2000;
    TEST_ASSERT_EQUAL_HEX32(0x100u, sweep_at(t));
    t += PARKED_RETRY_SWEEP_MS;
    TEST_ASSERT_EQUAL_HEX32(0x200u, sweep_at(t));
    t += PARKED_RETRY_SWEEP_MS;
    TEST_ASSERT_EQUAL_HEX32(0x300u, sweep_at(t));
    t += PARKED_RETRY_SWEEP_MS;
    TEST_ASSERT_EQUAL_HEX32(0x100u, sweep_at(t)); /* wrapped, nobody starved */
}

/* 3k. Nothing parked: the sweep costs one selection pass per interval and
 * never a flush. It is the flushes that are counted here, since those are what
 * cost airtime and a walk of the store's rows; the selection pass itself is
 * the sweep's fixed price and is not what this pins. */
void test_the_sweep_never_flushes_when_nothing_is_parked(void) {
    const uint32_t chatty = 0x1111FFFFu;
    beacon_from(chatty, 1000);
    int before = s_store_scans;

    run_an_hour_from(60000, chatty);

    TEST_ASSERT_EQUAL_INT_MESSAGE(before, s_store_scans,
                                  "an idle node must not flush anything on the sweep");
}

/* 4. The no-parked-messages case, which is nearly all of the time. */
void test_a_known_peer_with_nothing_parked_never_scans_the_store(void) {
    const uint32_t peer = 0xEE550055u;

    beacon_from(peer, 1000); /* admission scans once and finds nothing */
    int before = s_store_scans;

    for (uint32_t t = 61000; t <= 61000 + 20 * 60000; t += 60000)
        TEST_ASSERT_EQUAL_INT(-1, beacon_from(peer, t));

    TEST_ASSERT_EQUAL_INT_MESSAGE(before, s_store_scans,
                                  "a beacon from a peer with nothing parked must not touch "
                                  "the message store");
}

/* 5. Lazy disarm: a row can leave the parked state without telling anyone. */
void test_a_flush_that_finds_nothing_disarms_the_peer(void) {
    const uint32_t peer = 0x11660066u;

    beacon_from(peer, 1000);
    uint32_t uid = park_dm_for(peer, "on second thought", 2000);
    TEST_ASSERT_TRUE(msg_store_unpark(uid)); /* the user cancels it */

    /* One scan learns there is nothing left ... */
    TEST_ASSERT_EQUAL_INT(0, beacon_from(peer, 3000));
    int before = s_store_scans;

    /* ... and the peer is quiet from then on, well past a cooldown. */
    for (uint32_t t = 63000; t <= 63000 + 3600000; t += 60000)
        TEST_ASSERT_EQUAL_INT(-1, beacon_from(peer, t));
    TEST_ASSERT_EQUAL_INT(before, s_store_scans);
}

/* 6. The same lazy disarm after the message actually goes out. */
void test_delivering_the_last_parked_message_stops_the_retries(void) {
    const uint32_t peer = 0x22770077u;

    beacon_from(peer, 1000);
    uint32_t uid = park_dm_for(peer, "made it", 2000);
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 3000));
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, status_of(uid));

    /* The successful flush rearmed the cooldown, so one more scan happens and
     * finds nothing. After that the peer is quiet. */
    TEST_ASSERT_EQUAL_INT(0, beacon_from(peer, 3000 + PARKED_RETRY_COOLDOWN_MS));
    int before = s_store_scans;
    for (uint32_t t = 3000 + PARKED_RETRY_COOLDOWN_MS + 60000; t <= 10000000; t += 60000)
        TEST_ASSERT_EQUAL_INT(-1, beacon_from(peer, t));
    TEST_ASSERT_EQUAL_INT(before, s_store_scans);
}

/* 7. A fresh park is fresh user intent and does not wait out the cooldown. */
void test_parking_another_message_does_not_wait_out_the_cooldown(void) {
    const uint32_t peer = 0x33880088u;
    s_resend_succeeds = false;

    beacon_from(peer, 1000);
    uint32_t uid1 = park_dm_for(peer, "first", 2000);
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 3000));  /* attempt, stays parked */
    TEST_ASSERT_EQUAL_INT(-1, beacon_from(peer, 4000)); /* cooling down */

    uint32_t uid2 = park_dm_for(peer, "second", 5000);
    TEST_ASSERT_EQUAL_INT(2, beacon_from(peer, 6000));

    /* Both are now waiting on the same route or session, and it arrives. */
    send_queue_drains();
    TEST_ASSERT_EQUAL(MSG_STATUS_DELIVERED, status_of(uid2));
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, delivered_count(uid1),
                                  "parking a second message re-armed the peer, the next flush "
                                  "re-sent a row that was already sitting in the send queue, and "
                                  "the peer received one written message twice");
}

/* 7b. The other way two attempts can land inside the queue's window: the peer
 * is admitted again. is_new_peer flushes unconditionally, by design, and in a
 * table at capacity a peer is readmitted by rotation rather than by anyone
 * being away for the ten minutes a purge needs, so "again" can be a minute and
 * a half later. */
void test_a_readmitted_peer_cannot_re_send_what_is_still_in_the_queue(void) {
    const uint32_t peer = 0x00000001u;
    s_resend_succeeds = false;

    beacon_from(peer, 1000);
    uint32_t uid = park_dm_for(peer, "queued and waiting", 2000);
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 3000)); /* attempt: now in the queue */

    /* The table fills and this peer, the least recently heard, loses its slot
     * along with the armed field that lived in it. */
    for (uint32_t i = 0; i < MAX_NEIGHBORS; i++)
        beacon_from(0x1000u + i, 4000 + i * 10);
    TEST_ASSERT_NULL(neighbor_lookup(&s_nb, peer));

    /* It is readmitted 88 seconds later, the N=100 readmission interval, well
     * inside the window its first attempt is still queued in. */
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, 91000));

    send_queue_drains();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, delivered_count(uid),
                                  "a readmitted peer re-sent a row that was already sitting in "
                                  "the send queue, and received one written message twice");
}

/* 8. Arming a peer that is not in the table is a no-op, not a crash. */
void test_arming_an_absent_peer_is_a_no_op(void) {
    TEST_ASSERT_FALSE(parked_retry_arm(&s_nb, 0x44990099u, 1000));
    TEST_ASSERT_EQUAL_INT(0, neighbor_count(&s_nb));
}

/* 9. now_ms is a wrapping 32-bit uptime (mesh_task.c now_ms), so a cooldown
 * that straddles the wrap must still expire. */
void test_the_cooldown_survives_the_uptime_clock_wrapping(void) {
    const uint32_t peer = 0x5AA0AA00u;
    const uint32_t near_wrap = 0xFFFFF000u;
    s_resend_succeeds = false;

    beacon_from(peer, near_wrap);
    park_dm_for(peer, "across the wrap", near_wrap + 100);
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, near_wrap + 200));

    /* The rearmed deadline is near_wrap + 200 + PARKED_RETRY_COOLDOWN_MS,
     * which wraps past zero. */
    uint32_t due = near_wrap + 200 + PARKED_RETRY_COOLDOWN_MS;
    TEST_ASSERT_TRUE(due < near_wrap); /* it really did wrap */
    TEST_ASSERT_EQUAL_INT(-1, beacon_from(peer, due - 1));
    TEST_ASSERT_EQUAL_INT(1, beacon_from(peer, due));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_parked_message_is_delivered_when_the_peer_never_left);
    RUN_TEST(test_a_stuck_peer_gets_one_attempt_per_cooldown);
    RUN_TEST(test_rejoin_edge_still_flushes_a_peer_that_was_never_in_the_table);
    RUN_TEST(test_rejoin_edge_still_flushes_a_peer_that_left_and_returned);
    RUN_TEST(test_a_failed_rejoin_flush_still_leaves_a_trigger);
    RUN_TEST(test_a_partial_flush_keeps_a_trigger_for_the_row_left_behind);
    RUN_TEST(test_a_peer_readmitted_into_a_full_table_still_flushes);
    RUN_TEST(test_a_parked_message_reaches_a_peer_that_is_not_a_neighbour);
    RUN_TEST(test_the_sweep_attempts_a_stuck_peer_once_per_sweep_interval);
    RUN_TEST(test_a_peer_the_sweep_just_tried_is_not_immediately_retried_by_its_beacon);
    RUN_TEST(test_the_sweep_passes_over_a_direct_neighbour_and_moves_on);
    RUN_TEST(test_a_peer_the_sweep_passed_over_is_not_held_by_the_previous_peers_hold);
    RUN_TEST(test_the_sweep_rescues_a_neighbour_whose_arming_was_lost);
    RUN_TEST(test_an_unacknowledged_attempt_leaves_the_message_parked);
    RUN_TEST(test_a_flush_does_not_re_send_a_row_still_awaiting_an_ack);
    RUN_TEST(test_the_sweep_gives_each_parked_peer_a_turn);
    RUN_TEST(test_the_sweep_never_flushes_when_nothing_is_parked);
    RUN_TEST(test_a_known_peer_with_nothing_parked_never_scans_the_store);
    RUN_TEST(test_a_flush_that_finds_nothing_disarms_the_peer);
    RUN_TEST(test_delivering_the_last_parked_message_stops_the_retries);
    RUN_TEST(test_parking_another_message_does_not_wait_out_the_cooldown);
    RUN_TEST(test_a_readmitted_peer_cannot_re_send_what_is_still_in_the_queue);
    RUN_TEST(test_arming_an_absent_peer_is_a_no_op);
    RUN_TEST(test_the_cooldown_survives_the_uptime_clock_wrapping);
    return UNITY_END();
}
