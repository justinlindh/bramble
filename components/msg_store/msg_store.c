#include "msg_store.h"
#include <string.h>

/* The CONFIG_BRAMBLE_MSG_PERSIST_* values below come from here. Include it
 * explicitly rather than relying on esp_log.h to drag it in: that transitive
 * path only exists inside the ESP_PLATFORM branch, so on other platforms
 * persistence silently compiled itself out (found on the nRF target, where
 * the message store looked healthy and simply never wrote to flash). */
#include "sdkconfig.h"

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif

#ifdef CONFIG_BRAMBLE_MSG_PERSIST_ENABLED
#include "msg_store_spiffs.h"
#endif

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
static stored_msg_t* s_msgs = NULL;
/* The mesh task (core 1) writes the ring while the UI task (core 0) reads it.
 * A spinlock gives cross-core mutual exclusion around the short ring mutations
 * and the get-and-copy path so the UI never observes a half-written slot. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
#define MSG_LOCK() portENTER_CRITICAL(&s_lock)
#define MSG_UNLOCK() portEXIT_CRITICAL(&s_lock)
#elif defined(BRAMBLE_PLATFORM_NRF)
#include <FreeRTOS.h>
#include <semphr.h>
#include <task.h>
/* The mesh task writes this ring while the RPC transport task reads it
 * (bramble.getMessages over BLE), and a record is ~700 bytes, so an
 * unsynchronized reader can observe a half-written slot. ESP uses a
 * cross-core spinlock; a mutex is the single-core equivalent. Nothing here
 * runs in ISR context. */
static stored_msg_t s_msgs_storage[MSG_STORE_MAX];
static stored_msg_t* s_msgs = s_msgs_storage;
static SemaphoreHandle_t s_msg_lock;
static StaticSemaphore_t s_msg_lock_buf;
static void msg_lock_init(void) {
    if (s_msg_lock == NULL) {
        s_msg_lock = xSemaphoreCreateMutexStatic(&s_msg_lock_buf);
    }
}
static void msg_lock(void) {
    msg_lock_init();
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xSemaphoreTake(s_msg_lock, portMAX_DELAY);
    }
}
static void msg_unlock(void) {
    if (s_msg_lock != NULL && xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        xSemaphoreGive(s_msg_lock);
    }
}
#define MSG_LOCK() msg_lock()
#define MSG_UNLOCK() msg_unlock()

#else

static stored_msg_t s_msgs_storage[MSG_STORE_MAX];
static stored_msg_t* s_msgs = s_msgs_storage;
/* Host build is single-threaded test code; no locking needed. */
#define MSG_LOCK() ((void)0)
#define MSG_UNLOCK() ((void)0)
#endif

static void msg_store_ensure_alloc(void) {
#ifdef ESP_PLATFORM
    if (s_msgs)
        return;
    /* PSRAM-first: ~700 B per slot makes large caps unaffordable in
     * internal SRAM. Fall back to the default heap on PSRAM-less boards
     * (their cap stays small via the Kconfig default). */
    s_msgs = heap_caps_calloc(MSG_STORE_MAX, sizeof(stored_msg_t), MALLOC_CAP_SPIRAM);
    if (!s_msgs)
        s_msgs = heap_caps_calloc(MSG_STORE_MAX, sizeof(stored_msg_t), MALLOC_CAP_DEFAULT);
    if (!s_msgs) {
        ESP_LOGE("msg_store", "ring alloc failed (%u slots x %u B); messages will be dropped",
                 (unsigned)MSG_STORE_MAX, (unsigned)sizeof(stored_msg_t));
    }
#endif
}
static int s_head = 0;                /* Next write position */
static int s_count = 0;               /* Number of stored messages */
static uint32_t s_total_incoming = 0; /* Monotonic incoming counter, survives ring wrap */
static uint32_t s_next_uid = 1;       /* Stable per-message id allocator (0 = untracked) */

static uint32_t get_uptime_s(void) {
#ifdef ESP_PLATFORM
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
#else
    return 0;
#endif
}

void msg_store_init(void) {
    msg_store_ensure_alloc();
    if (!s_msgs)
        return;
    memset(s_msgs, 0, MSG_STORE_MAX * sizeof(stored_msg_t));
    s_head = 0;
    s_count = 0;
    s_total_incoming = 0;
    s_next_uid = 1;
}

uint32_t msg_store_next_uid(void) {
    MSG_LOCK();
    uint32_t uid = s_next_uid++;
    if (s_next_uid == 0)
        s_next_uid = 1; /* wrap past the reserved "untracked" value */
    MSG_UNLOCK();
    return uid;
}

static void msg_store_add_full(uint32_t peer_addr, msg_direction_t dir, const char* text,
                               size_t text_len, int8_t rssi, int8_t snr, uint32_t packet_id,
                               msg_status_t status, int16_t channel_index, uint32_t uid) {
    /* Lazy retry: a failed boot-time alloc may succeed once the heap
     * settles, instead of dropping messages for the whole session. */
    msg_store_ensure_alloc();
    if (!s_msgs)
        return;
    if (text_len >= MSG_TEXT_MAX) {
        text_len = MSG_TEXT_MAX - 1;
    }

    /* Every row carries a uid, allocated here for the callers that have none
     * of their own. What uid 0 means to a caller is untouched: nobody knows an
     * id allocated in here, so nothing can target such a row with
     * msg_store_update_by_uid. What it buys is that every persisted record
     * holds an id unique within the ring, which is how the persistence backend
     * tells one record from another when it confirms a status update is
     * landing on the right one. Allocated before the lock is taken: the
     * allocator takes the same non-recursive lock. */
    if (uid == 0)
        uid = msg_store_next_uid();

    /* Hold the lock across the whole slot write so a concurrent UI reader
     * copies either the old slot or the fully-written new one, never a torn
     * mix. SPIFFS persistence below runs after the unlock (writers are
     * single-task, so m stays stable for it). */
    stored_msg_t* m = &s_msgs[s_head];
    MSG_LOCK();
    memset(m, 0, sizeof(*m));
    m->peer_addr = peer_addr;
    m->uid = uid;
    m->direction = dir;
    m->status = status;
    m->packet_id = packet_id;
    m->timestamp_s = get_uptime_s();
    m->rssi = rssi;
    m->snr = snr;
    m->channel_index = channel_index;

    memcpy(m->text, text, text_len);
    m->text[text_len] = '\0';
    m->text_len = (uint16_t)text_len;

    s_head = (s_head + 1) % MSG_STORE_MAX;
    if (s_count < MSG_STORE_MAX) {
        s_count++;
    }
    if (dir == MSG_DIR_INCOMING || dir == MSG_DIR_BROADCAST_IN) {
        s_total_incoming++;
    }
    MSG_UNLOCK();

#ifdef CONFIG_BRAMBLE_MSG_PERSIST_ENABLED
    /* Persist to SPIFFS */
    if (msg_store_spiffs_save(m) == 0) {
        /* Check if rollover needed */
        int total = msg_store_spiffs_get_count();
        if (total >= CONFIG_BRAMBLE_MSG_PERSIST_MAX) {
            msg_store_spiffs_rollover(CONFIG_BRAMBLE_MSG_PERSIST_MAX,
                                      CONFIG_BRAMBLE_MSG_PERSIST_ROLLOVER_KEEP_PCT);
        }
    }
#endif
}

void msg_store_add_ex2(uint32_t peer_addr, msg_direction_t dir, const char* text, size_t text_len,
                       int8_t rssi, int8_t snr, uint32_t packet_id, msg_status_t status,
                       int16_t channel_index) {
    msg_store_add_full(peer_addr, dir, text, text_len, rssi, snr, packet_id, status, channel_index,
                       0);
}

void msg_store_add_ex(uint32_t peer_addr, msg_direction_t dir, const char* text, size_t text_len,
                      int8_t rssi, int8_t snr, uint32_t packet_id, msg_status_t status) {
    msg_store_add_ex2(peer_addr, dir, text, text_len, rssi, snr, packet_id, status,
                      MSG_STORE_DM_CHANNEL);
}

void msg_store_add_dm(uint32_t peer_addr, msg_direction_t dir, const char* text, size_t text_len,
                      int8_t rssi, int8_t snr, uint32_t packet_id, msg_status_t status) {
    msg_store_add_dm_uid(peer_addr, dir, text, text_len, rssi, snr, packet_id, status, 0);
}

void msg_store_add_dm_uid(uint32_t peer_addr, msg_direction_t dir, const char* text,
                          size_t text_len, int8_t rssi, int8_t snr, uint32_t packet_id,
                          msg_status_t status, uint32_t uid) {
    msg_store_add_full(peer_addr, dir, text, text_len, rssi, snr, packet_id, status,
                       MSG_STORE_DM_CHANNEL, uid);
}

void msg_store_add_channel(uint32_t peer_addr, msg_direction_t dir, const char* text,
                           size_t text_len, int8_t rssi, int8_t snr, uint32_t packet_id,
                           msg_status_t status, uint8_t channel_index) {
    /* channel_index is non-negative by type: a negative "DM" index cannot be
     * expressed here, so channel traffic can never be filed channel-less. */
    msg_store_add_ex2(peer_addr, dir, text, text_len, rssi, snr, packet_id, status,
                      (int16_t)channel_index);
}

void msg_store_add(uint32_t peer_addr, msg_direction_t dir, const char* text, size_t text_len,
                   int8_t rssi, int8_t snr) {
    msg_store_add_ex(peer_addr, dir, text, text_len, rssi, snr, 0, MSG_STATUS_NONE);
}

/*
 * Push a row whose delivery state just changed back to its persisted record,
 * so a reboot restores "delivered" instead of the status the message was
 * first stored with. Runs AFTER the ring lock is released (flash I/O must
 * never happen inside a critical section), and takes the ring index rather
 * than a copy for the same reason msg_store_add_full hands the backend a
 * pointer into the ring: writers are single-task, so the row stays put.
 *
 * from_end is the row's distance from the newest, which is how the ring maps
 * onto the file: both hold a suffix of the same append sequence. The backend
 * re-checks that the record it lands on is really this message, so a mapping
 * skewed by a failed append costs an unpersisted status, never a corrupted
 * neighbour.
 *
 * Only a change is written. Repeat delivery receipts for one broadcast all
 * report DELIVERED, and a message sees at most a couple of real transitions,
 * so the flash cost is a handful of record rewrites per message rather than
 * one per receipt.
 */
static void persist_row_update(int idx, int from_end) {
#ifdef CONFIG_BRAMBLE_MSG_PERSIST_ENABLED
    msg_store_spiffs_update(from_end, &s_msgs[idx]);
#else
    (void)idx;
    (void)from_end;
#endif
}

bool msg_store_update_status_with_route(uint32_t packet_id, msg_status_t status,
                                        uint8_t route_hop_count, const uint32_t* route_hops) {
    if (packet_id == 0)
        return false;
    bool found = false;
    bool changed = false;
    int found_idx = 0;
    int from_end = 0;
    MSG_LOCK();
    int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
    /* Search newest first for faster match */
    for (int i = s_count - 1; i >= 0; i--) {
        int idx = (start + i) % MSG_STORE_MAX;
        if (s_msgs && s_msgs[idx].packet_id == packet_id) {
            /* Parked is sticky here too, on exactly the rule
             * msg_store_update_by_uid's doc comment states: DELIVERED is the
             * only status a QUEUED row accepts. This is the path that would
             * otherwise strand a parked message: the ACK retry tick reports
             * FAILED against a packet_id, and a parked row marked SENT by its
             * own retry would no longer be QUEUED when that lands, so this
             * guard would not fire and the row would leave the parked state
             * for good. Keeping the row QUEUED through the transmit is what
             * makes this guard the one that catches it. */
            bool sticky = s_msgs[idx].status == MSG_STATUS_QUEUED && status != MSG_STATUS_DELIVERED;
            changed = !sticky && s_msgs[idx].status != status;
            if (!sticky)
                s_msgs[idx].status = status;
            if (route_hops && route_hop_count > 0) {
                uint8_t bounded =
                    (route_hop_count > MSG_ROUTE_MAX_HOPS) ? MSG_ROUTE_MAX_HOPS : route_hop_count;
                /* A route arriving for a row that had none is worth persisting
                 * too; a second receipt reporting a different path for the same
                 * delivered message is not. */
                changed = changed || s_msgs[idx].route_hop_count == 0;
                s_msgs[idx].route_hop_count = bounded;
                for (uint8_t h = 0; h < bounded; h++) {
                    s_msgs[idx].route_hops[h] = route_hops[h];
                }
            }
            found = true;
            found_idx = idx;
            from_end = s_count - 1 - i;
            break;
        }
    }
    MSG_UNLOCK();
    if (found && changed)
        persist_row_update(found_idx, from_end);
    return found;
}

bool msg_store_update_status(uint32_t packet_id, msg_status_t status) {
    return msg_store_update_status_with_route(packet_id, status, 0, NULL);
}

bool msg_store_update_by_uid(uint32_t uid, uint32_t packet_id, msg_status_t status) {
    if (uid == 0)
        return false;
    msg_store_ensure_alloc();
    if (!s_msgs)
        return false;
    bool found = false;
    bool changed = false;
    int found_idx = 0;
    int from_end = 0;
    MSG_LOCK();
    int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
    /* Search newest first: the row being reconciled is almost always recent */
    for (int i = s_count - 1; i >= 0; i--) {
        int idx = (start + i) % MSG_STORE_MAX;
        if (s_msgs[idx].uid == uid) {
            if (packet_id != 0)
                s_msgs[idx].packet_id = packet_id;
            /* The wire packet_id this stamps rides along on whatever write a
             * status change earns; it is correlation state, not something a
             * reboot has any use for on its own. */
            /* Parked is sticky: DELIVERED is the ONLY status a QUEUED row
             * accepts (see the doc comment on this function). A send attempt
             * must never un-park a row, and the packet_id stamped just above
             * is what lets the attempt's eventual ACK still find it. Only
             * msg_store_unpark() may move a QUEUED row out any other way. */
            bool sticky = s_msgs[idx].status == MSG_STATUS_QUEUED && status != MSG_STATUS_DELIVERED;
            changed = !sticky && s_msgs[idx].status != status;
            if (!sticky)
                s_msgs[idx].status = status;
            found = true;
            found_idx = idx;
            from_end = s_count - 1 - i;
            break;
        }
    }
    MSG_UNLOCK();
    if (found && changed)
        persist_row_update(found_idx, from_end);
    return found;
}

bool msg_store_unpark(uint32_t uid) {
    if (uid == 0)
        return false;
    msg_store_ensure_alloc();
    if (!s_msgs)
        return false;
    bool found = false;
    bool changed = false;
    int found_idx = 0;
    int from_end = 0;
    MSG_LOCK();
    int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
    for (int i = s_count - 1; i >= 0; i--) {
        int idx = (start + i) % MSG_STORE_MAX;
        if (s_msgs[idx].uid == uid) {
            found = s_msgs[idx].status == MSG_STATUS_QUEUED;
            if (found) {
                s_msgs[idx].status = MSG_STATUS_FAILED;
                changed = true;
                found_idx = idx;
                from_end = s_count - 1 - i;
            }
            break;
        }
    }
    MSG_UNLOCK();
    if (found && changed)
        persist_row_update(found_idx, from_end);
    return found;
}

/* An outgoing DM currently parked for a peer. The shared half of what all three
 * parked-row walks below select on. */
static bool is_parked_dm(const stored_msg_t* m) {
    return m->direction == MSG_DIR_OUTGOING && m->channel_index < 0 &&
           m->status == MSG_STATUS_QUEUED && m->uid != 0;
}

/* Is this row past the park window, measured from when it was STORED?
 *
 * Not "how long it has been parked": timestamp_s is stamped once by
 * msg_store_add_full and nothing restamps it, so this is the message's age. Two
 * callers depend on that being the same question. A row already parked is asked
 * whether to keep retrying it, and a row about to be parked is asked whether
 * parking it could achieve anything, and the second only works because both
 * measure from the same origin. Parking a row that is already past the window
 * would otherwise promise a retry the expiry pass cancels minutes later without
 * ever attempting it.
 *
 * Exactly at the TTL is still inside it, so the window means what it says.
 *
 * A row stamped ahead of now_s is treated as NOT past the window rather than as
 * enormously old. Unsigned subtraction would turn that into a huge age and
 * expire a message the user just parked, which is the worse direction to be
 * wrong in by far. It should not arise (restored rows are zeroed on load and a
 * fresh stamp cannot exceed a later read of the same clock), so this is a guard
 * against a future clock change, not a live case. */
static bool past_park_window(const stored_msg_t* m, uint32_t now_s) {
    if (now_s < m->timestamp_s)
        return false;
    return (now_s - m->timestamp_s) > MSG_STORE_PARK_TTL_S;
}

bool msg_store_park_window_open(uint32_t uid, uint32_t now_s) {
    if (uid == 0)
        return false;
    msg_store_ensure_alloc();
    if (!s_msgs)
        return false;
    bool open = false;
    MSG_LOCK();
    int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
    for (int i = s_count - 1; i >= 0; i--) {
        int idx = (start + i) % MSG_STORE_MAX;
        if (s_msgs[idx].uid == uid) {
            open = !past_park_window(&s_msgs[idx], now_s);
            break;
        }
    }
    MSG_UNLOCK();
    return open;
}

int msg_store_expire_parked(uint32_t now_s) {
    msg_store_ensure_alloc();
    if (!s_msgs)
        return 0;
    int expired = 0;
    /* One row per lock cycle, the same shape every other status write in this
     * file uses: take the lock, change one row, release, then persist outside
     * it, because persistence is flash I/O and this lock is a spinlock on the
     * ESP target. Repeating until a pass finds nothing needs no buffer of
     * pending indices, which matters because this can be reached from the mesh
     * task and MSG_STORE_MAX rows' worth of scratch does not belong on that
     * stack. Normally the first pass finds nothing at all. */
    for (;;) {
        bool found = false;
        int found_idx = 0;
        int from_end = 0;
        MSG_LOCK();
        int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
        for (int i = 0; i < s_count; i++) {
            int idx = (start + i) % MSG_STORE_MAX;
            stored_msg_t* m = &s_msgs[idx];
            if (!is_parked_dm(m) || !past_park_window(m, now_s)) {
                continue;
            }
            /* The same door msg_store_unpark uses, and for the same reason:
             * the sticky rule refuses QUEUED -> FAILED from a send path, which
             * is right, and giving up on age is not a send failing. */
            m->status = MSG_STATUS_FAILED;
            found = true;
            found_idx = idx;
            from_end = s_count - 1 - i;
            break;
        }
        MSG_UNLOCK();
        if (!found)
            break;
        persist_row_update(found_idx, from_end);
        expired++;
    }
    return expired;
}

int msg_store_parked_uids_for_peer(uint32_t peer_addr, uint32_t* out_uids, int max_out,
                                   uint32_t now_s) {
    if (!out_uids || max_out <= 0) {
        return 0;
    }
    int n = 0;
    MSG_LOCK();
    if (s_msgs) {
        /* The ring is circular: index 0 is the OLDEST row and lives at
         * s_head - s_count, not at s_msgs[0]. Same walk msg_store_get_copy
         * does. Oldest first, because a parked conversation should arrive in
         * the order it was written. */
        int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
        for (int i = 0; i < s_count && n < max_out; i++) {
            const stored_msg_t* m = &s_msgs[(start + i) % MSG_STORE_MAX];
            if (!is_parked_dm(m) || m->peer_addr != peer_addr) {
                continue;
            }
            /* Skipped, not rewritten: this is a read. msg_store_expire_parked
             * is what makes an overdue row visibly failed. Skipping here too
             * means the beacon path cannot re-send one in the window between
             * its TTL passing and the next expiry pass. */
            if (past_park_window(m, now_s)) {
                continue;
            }
            out_uids[n++] = m->uid;
        }
    }
    MSG_UNLOCK();
    return n;
}

bool msg_store_next_parked_peer(uint32_t after_peer_addr, uint32_t* out_peer_addr, uint32_t now_s) {
    if (!out_peer_addr) {
        return false;
    }
    bool found_any = false;
    uint32_t lowest = 0;     /* lowest parked peer overall, for the wrap */
    uint32_t next_above = 0; /* lowest parked peer above after_peer_addr */
    bool found_above = false;
    MSG_LOCK();
    if (s_msgs) {
        int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
        for (int i = 0; i < s_count; i++) {
            const stored_msg_t* m = &s_msgs[(start + i) % MSG_STORE_MAX];
            if (!is_parked_dm(m) || past_park_window(m, now_s)) {
                continue;
            }
            if (!found_any || m->peer_addr < lowest) {
                lowest = m->peer_addr;
                found_any = true;
            }
            if (m->peer_addr > after_peer_addr && (!found_above || m->peer_addr < next_above)) {
                next_above = m->peer_addr;
                found_above = true;
            }
        }
    }
    MSG_UNLOCK();
    if (!found_any) {
        return false;
    }
    *out_peer_addr = found_above ? next_above : lowest;
    return true;
}

bool msg_store_peer_for_uid(uint32_t uid, uint32_t* out_peer_addr) {
    if (!out_peer_addr || uid == 0) {
        return false;
    }
    bool ok = false;
    MSG_LOCK();
    if (s_msgs) {
        int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
        for (int i = 0; i < s_count; i++) {
            const stored_msg_t* m = &s_msgs[(start + i) % MSG_STORE_MAX];
            if (m->uid == uid) {
                *out_peer_addr = m->peer_addr;
                ok = true;
                break;
            }
        }
    }
    MSG_UNLOCK();
    return ok;
}

bool msg_store_get_copy_by_uid(uint32_t uid, stored_msg_t* out) {
    if (!out || uid == 0) {
        return false;
    }
    bool ok = false;
    MSG_LOCK();
    if (s_msgs) {
        int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
        for (int i = 0; i < s_count; i++) {
            const stored_msg_t* m = &s_msgs[(start + i) % MSG_STORE_MAX];
            if (m->uid == uid) {
                *out = *m;
                ok = true;
                break;
            }
        }
    }
    MSG_UNLOCK();
    return ok;
}

int msg_store_count(void) {
    MSG_LOCK();
    int c = s_count;
    MSG_UNLOCK();
    return c;
}

uint32_t msg_store_total_incoming(void) { return s_total_incoming; }

uint32_t msg_store_count_outgoing_delivered(void) {
    uint32_t c = 0;
    MSG_LOCK();
    int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
    for (int i = 0; i < s_count; i++) {
        const stored_msg_t* m = &s_msgs[(start + i) % MSG_STORE_MAX];
        if (m->direction == MSG_DIR_OUTGOING && m->status == MSG_STATUS_DELIVERED)
            c++;
    }
    MSG_UNLOCK();
    return c;
}

const stored_msg_t* msg_store_get(int index) {
    if (index < 0 || index >= s_count)
        return NULL;

    int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
    int actual = (start + index) % MSG_STORE_MAX;
    return &s_msgs[actual];
}

bool msg_store_get_copy(int index, stored_msg_t* out) {
    if (!out)
        return false;
    bool ok = false;
    MSG_LOCK();
    if (s_msgs && index >= 0 && index < s_count) {
        int start = (s_head - s_count + MSG_STORE_MAX) % MSG_STORE_MAX;
        int actual = (start + index) % MSG_STORE_MAX;
        *out = s_msgs[actual];
        ok = true;
    }
    MSG_UNLOCK();
    return ok;
}

void msg_store_init_with_persistence(void) {
    msg_store_init();

#ifdef CONFIG_BRAMBLE_MSG_PERSIST_ENABLED
    /* Initialize SPIFFS persistence */
    if (msg_store_spiffs_init() == 0) {
        /*
         * ROOT-CAUSE FIX:
         * Do NOT allocate stored_msg_t loaded[MSG_STORE_MAX] on stack here.
         * app_main runs with a small default main-task stack (often 3584 bytes).
         * A local loaded[] buffer is ~4.4KB (20 * sizeof(stored_msg_t)) and can
         * overflow main-task stack, causing later boot-stage crashes.
         */
        int count = msg_store_spiffs_load_recent(s_msgs, MSG_STORE_MAX);
        if (count < 0)
            count = 0;
        if (count > MSG_STORE_MAX)
            count = MSG_STORE_MAX;

        s_count = count;
        s_head = count % MSG_STORE_MAX;

        /* Restored timestamps are a PREVIOUS boot's uptime clock and would
         * render as garbage ages; zero them so the UI hides the age.
         *
         * The uid allocator resumes past the highest restored uid in the same
         * pass. Restarting it at 1 would hand a fresh message a uid a restored
         * row in the same ring already holds, and uid is what identifies a row
         * to msg_store_update_by_uid and to the persistence drift check. */
        for (int i = 0; i < count; i++) {
            s_msgs[i].timestamp_s = 0;
            if (s_msgs[i].uid >= s_next_uid) {
                s_next_uid = s_msgs[i].uid + 1;
                if (s_next_uid == 0)
                    s_next_uid = 1;
            }
        }
    }
#endif
}
