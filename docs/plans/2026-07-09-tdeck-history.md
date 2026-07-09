# T-Deck History Batch (B3) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The chat UI shows real history: a 200-message window on the T-Deck (PSRAM) instead of 20, chat-list DM rows ordered by recency and no longer vanishing, and conversation rendering bounded so deep channels stay responsive.

**Architecture:** `MSG_STORE_MAX` becomes Kconfig-driven (`CONFIG_BRAMBLE_MSG_STORE_CAP`, default 20; 200 on the T-Deck). The ring moves from a static array to a once-allocated buffer (PSRAM-first on ESP, static on host). Boot already loads `MSG_STORE_MAX` recent messages from SPIFFS (`msg_store_init_with_persistence`), so the deeper window fills from the 300-message archive automatically. The chat UI gets recency-ordered DM derivation and a render bound.

**Tech Stack:** ESP-IDF C, Unity host tests.

## Global Constraints

- No em dashes; no AI attribution; conventional commits (house rules).
- Branch: `feat/tdeck-history` off `origin/main` (already created).
- Gates: `bash test/run_all_tests.sh`, plus tdeck-plus, heltec-v3, heltec-v4 builds (msg_store is shared by all boards).
- `stored_msg_t` is ~700 bytes (`MSG_TEXT_MAX` 640): 200 slots is ~140 KB and MUST NOT be a static array (internal SRAM); allocate PSRAM-first at init.
- Only `components/msg_store` and `test/test_msg_store.c` reference `MSG_STORE_MAX` (verified by grep); no other consumer assumptions to fix.

---

### Task 1: Kconfig-driven capacity + PSRAM allocation

**Files:**
- Modify: `components/msg_store/include/msg_store.h` (lines 12: cap define)
- Modify: `components/msg_store/msg_store.c` (ring becomes allocated)
- Modify: `main/Kconfig.projbuild` (new option next to `BRAMBLE_MSG_PERSIST_MAX`)
- Modify: `sdkconfig.defaults.tdeck_plus` (set 200)
- Test: `test/test_msg_store.c`

**Interfaces:** `MSG_STORE_MAX` keeps its name and all existing semantics; only its value source and the ring's storage change. No API changes.

- [ ] **Step 1: Failing test (windowing across the cap)**

Add to `test/test_msg_store.c` (register in `main`):

```c
void test_ring_keeps_newest_window_at_capacity(void) {
    msg_store_init();
    char text[16];
    for (int i = 0; i < MSG_STORE_MAX + 5; i++) {
        snprintf(text, sizeof(text), "m%d", i);
        msg_store_add(0x1111, MSG_DIR_INCOMING, text, strlen(text), -70, 5);
    }
    TEST_ASSERT_EQUAL(MSG_STORE_MAX, msg_store_count());
    /* Oldest retained message is number 5; newest is MSG_STORE_MAX + 4 */
    char expect[16];
    snprintf(expect, sizeof(expect), "m%d", 5);
    TEST_ASSERT_EQUAL_STRING(expect, msg_store_get(0)->text);
    snprintf(expect, sizeof(expect), "m%d", MSG_STORE_MAX + 4);
    TEST_ASSERT_EQUAL_STRING(expect, msg_store_get(msg_store_count() - 1)->text);
}
```

(Needs `#include <string.h>` and `#include <stdio.h>` in the test file if missing.) This passes already with the static ring; it pins the behavior the allocation change must preserve. Run it BEFORE the change to confirm green, keep it green after.

- [ ] **Step 2: Kconfig option**

In `main/Kconfig.projbuild`, next to the `BRAMBLE_MSG_PERSIST_*` options:

```
        config BRAMBLE_MSG_STORE_CAP
            int "In-RAM message ring capacity"
            default 20
            help
                Number of messages held in RAM for the UI. The buffer is
                allocated PSRAM-first at init (about 700 bytes per slot),
                so large values are only appropriate on PSRAM boards.
```

In `sdkconfig.defaults.tdeck_plus`, next to the persist settings:

```
CONFIG_BRAMBLE_MSG_STORE_CAP=200
```

- [ ] **Step 3: Header**

In `components/msg_store/include/msg_store.h`, replace `#define MSG_STORE_MAX 20` with:

```c
#if defined(ESP_PLATFORM)
#include "sdkconfig.h"
#endif

#ifdef CONFIG_BRAMBLE_MSG_STORE_CAP
#define MSG_STORE_MAX CONFIG_BRAMBLE_MSG_STORE_CAP
#else
#define MSG_STORE_MAX 20
#endif
```

(Host test builds have no `ESP_PLATFORM` and no stubs on the include path for this target, hence the guard.)

- [ ] **Step 4: Allocated ring**

In `components/msg_store/msg_store.c`, replace `static stored_msg_t s_msgs[MSG_STORE_MAX];` with:

```c
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
static stored_msg_t* s_msgs = NULL;
#else
static stored_msg_t s_msgs_storage[MSG_STORE_MAX];
static stored_msg_t* s_msgs = s_msgs_storage;
#endif

static void msg_store_ensure_alloc(void) {
#ifdef ESP_PLATFORM
    if (s_msgs)
        return;
    /* PSRAM-first: ~700 B per slot makes large caps unaffordable in
     * internal SRAM. Fall back to the default heap on PSRAM-less boards
     * (their cap stays small via Kconfig default). */
    s_msgs = heap_caps_calloc(MSG_STORE_MAX, sizeof(stored_msg_t), MALLOC_CAP_SPIRAM);
    if (!s_msgs)
        s_msgs = heap_caps_calloc(MSG_STORE_MAX, sizeof(stored_msg_t), MALLOC_CAP_DEFAULT);
#endif
}
```

In `msg_store_init()`: call `msg_store_ensure_alloc();` first, guard the memset (`if (!s_msgs) return;`), and change `memset(s_msgs, 0, sizeof(s_msgs));` to `memset(s_msgs, 0, MSG_STORE_MAX * sizeof(stored_msg_t));` (the pointer sizeof is wrong now).

In `msg_store_add_ex2()`: add `if (!s_msgs) return;` as the first statement (defensive on alloc failure).

`msg_store_init_with_persistence()` calls `msg_store_init()` first, so the buffer exists before `msg_store_spiffs_load_recent(s_msgs, MSG_STORE_MAX)` fills it; no change needed there beyond it inheriting the deeper cap.

- [ ] **Step 5: Gates**

```bash
cd test/build && cmake .. >/dev/null && make test_msg_store >/dev/null && ./test_msg_store
cd ../.. && bash test/run_all_tests.sh 2>&1 | tail -2
bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -1
bash scripts/flash.sh local heltec-v3 build 2>&1 | tail -1
bash scripts/flash.sh local heltec-v4 build 2>&1 | tail -1
```
Expected: all green (heltec boards stay at cap 20 via the Kconfig default; their alloc lands in the default heap, same ~14 KB as the old static).

- [ ] **Step 6: Commit**

```bash
git add components/msg_store main/Kconfig.projbuild sdkconfig.defaults.tdeck_plus test/test_msg_store.c
git commit -m "feat(msg_store): Kconfig-driven ring capacity, PSRAM-backed, 200 on T-Deck

The chat UI could only ever show the last 20 messages across ALL
conversations while 300 sat in the SPIFFS archive. The ring buffer is
now allocated PSRAM-first with a per-board capacity; the T-Deck gets a
200-message window that boot already fills from persistence."
```

---

### Task 2: Chat list DM rows: newest-first, more slots

**Files:**
- Modify: `components/ui_graphics/screens/scr_chat_list.c` (DM derivation loop)

**Why:** the DM section scans the store oldest-first and keeps the first 6 distinct peers, so old conversations crowd out active ones, and with the deeper store the bias worsens. Scan newest-first and allow 12 rows.

- [ ] **Step 1: Replace the derivation loop**

Change:

```c
    uint32_t dm_peers[6];
    int dm_count = 0;
    int msg_count = msg_store_count();
    for (int i = 0; i < msg_count && dm_count < 6; i++) {
```

to:

```c
    uint32_t dm_peers[12];
    int dm_count = 0;
    int msg_count = msg_store_count();
    /* Newest-first so active conversations rank above stale ones */
    for (int i = msg_count - 1; i >= 0 && dm_count < 12; i--) {
```

(The dedup inner loop and the card loop below use `dm_count` and `dm_peers[i]` unchanged; the `< 6` in the loop condition is the only other 6.)

- [ ] **Step 2: Build gate + commit**

```bash
bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -1
git add components/ui_graphics
git commit -m "fix(ui_gfx): chat list DM rows favor recent conversations

Derive DM rows newest-first (so active threads rank first) and allow
12 of them. With the deeper message window, conversations no longer
vanish the moment their last message left the tiny RAM ring."
```

---

### Task 3: Bound conversation rendering

**Files:**
- Modify: `components/ui_graphics/screens/scr_chat_messages.c` (`render_messages_for_target`)

**Why:** rendering builds LVGL widgets for every matching message; with a 200-deep store a busy channel would create hundreds of objects per re-render. Render only the newest `CHAT_RENDER_MAX` (60) matching messages.

- [ ] **Step 1: Rewrite the message loop**

In `render_messages_for_target`, replace the forward scan:

```c
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    int count = msg_store_count();
    for (int i = 0; i < count; i++) {
        const stored_msg_t* msg = msg_store_get(i);
        if (!msg || !message_matches_target(msg))
            continue;
```

with a bounded newest-first collection followed by in-order rendering:

```c
    uint32_t now_s = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    int count = msg_store_count();

    /* Collect the newest CHAT_RENDER_MAX matching messages, then render
     * them oldest-first. Keeps LVGL object count bounded on deep stores. */
#define CHAT_RENDER_MAX 60
    static int s_match_idx[CHAT_RENDER_MAX];
    int n_match = 0;
    for (int i = count - 1; i >= 0 && n_match < CHAT_RENDER_MAX; i--) {
        const stored_msg_t* m = msg_store_get(i);
        if (m && message_matches_target(m))
            s_match_idx[n_match++] = i;
    }

    for (int k = n_match - 1; k >= 0; k--) {
        const stored_msg_t* msg = msg_store_get(s_match_idx[k]);
        if (!msg)
            continue;
```

The loop body (is_mine, sender, action/bubble, age) stays identical; only the iteration header changes (and the closing brace count stays the same).

- [ ] **Step 2: Build gate + commit**

```bash
bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -1
git add components/ui_graphics
git commit -m "perf(ui_gfx): bound conversation rendering to the newest 60 messages

With a 200-message store a busy channel would otherwise build hundreds
of LVGL objects per re-render."
```

---

### Task 4: Full gate + PR

- [ ] Run the full gate (host suite, three boards), clang-format changed files, commit plan file, push `feat/tdeck-history`, open PR via Gitea API (`feat(ui_gfx): T-Deck history batch`), poll CI, report URL, STOP (merge needs user instruction).
