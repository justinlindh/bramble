# T-Deck Trust Batch (B2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every destructive action confirms, every mutating action gives visible feedback, and the status bar stops lying (real node name, real clock when timesync has one, live GPS state, real version string).

**Architecture:** One new LVGL helper (`ui_confirm`) mirroring `ui_toast`; feedback wired through the existing `ui_toast_show`. One new mesh getter (`mesh_get_network_time_ms`) following the `mesh_get_node_name` extern pattern. All edits inside `components/ui_graphics` plus a small `main/mesh_task.{c,h}` addition.

**Tech Stack:** ESP-IDF C, LVGL v9. Almost entirely LVGL glue; the only host-testable addition is trivial, so the gate is the tdeck build plus the existing host suite staying green.

## Global Constraints

- No em dashes anywhere; no AI attribution; conventional commits (house rules).
- Branch: `feat/tdeck-trust` off `origin/main` (already created).
- Gates: `bash scripts/flash.sh local tdeck-plus build` and `bash test/run_all_tests.sh`.
- Consumes from B1 (merged): `ui_toast_show(const char*)` from `ui_toast.h`.

## Findings this fixes

1. Reboot Device (`scr_settings.c` `reboot_cb`), Apply Mode & Reboot (`conn_apply_cb`), Panic Off (`location_panic_off_cb`), and channel remove (`channel_remove_cb`) all fire instantly on one tap; Apply Mode silently no-ops when unchanged.
2. Radio apply, channel add/set-default, and node-name save give no visible feedback; empty name/channel-name inputs are silently discarded.
3. Status bar: node name hardcoded "BRAMBLE" (`scr_layout.c:193-194`) though `mesh_get_node_name()` exists; clock stuck at "--:--" though timesync tracks network time; GPS label static though the T-Deck has `BOARD_CAP_GPS` and `gps_has_fix()` exists.
4. Settings version label is a hardcoded `"0.9.1-tdeck"` string (`scr_settings.c:1669`).
5. A DM with an unnamed peer titles the conversation "Chat" (`scr_chat_messages.c` `update_title`); the conversation target switcher is an invisible unlabeled button.

---

### Task 1: `ui_confirm` modal helper

**Files:**
- Create: `components/ui_graphics/include/ui_confirm.h`
- Create: `components/ui_graphics/ui_confirm.c`
- Modify: `components/ui_graphics/CMakeLists.txt` (add `"ui_confirm.c"` after `"ui_toast.c"`)

**Interfaces:**
- Produces:

```c
typedef void (*ui_confirm_cb_t)(void* user_data);
/* Modal yes/no. Confirm button carries confirm_label and danger color;
 * Cancel (focused by default) just closes. on_confirm runs after the
 * modal closes. One modal at a time; a second call replaces the first. */
void ui_confirm_show(const char* text, const char* confirm_label, ui_confirm_cb_t on_confirm,
                     void* user_data);
```

- [ ] **Step 1: Header**

```c
#ifndef BRAMBLE_UI_CONFIRM_H
#define BRAMBLE_UI_CONFIRM_H

typedef void (*ui_confirm_cb_t)(void* user_data);

void ui_confirm_show(const char* text, const char* confirm_label, ui_confirm_cb_t on_confirm,
                     void* user_data);

#endif
```

- [ ] **Step 2: Implementation** (`ui_confirm.c`)

```c
#include "ui_confirm.h"
#include "theme/bramble_theme.h"
#include "lvgl.h"

static lv_obj_t* s_overlay = NULL;
static ui_confirm_cb_t s_on_confirm = NULL;
static void* s_user_data = NULL;

static void confirm_close(void) {
    if (s_overlay) {
        lv_obj_delete(s_overlay); /* deletion drops children from the input group */
        s_overlay = NULL;
    }
}

static void cancel_cb(lv_event_t* e) {
    (void)e;
    confirm_close();
}

static void confirm_cb(lv_event_t* e) {
    (void)e;
    ui_confirm_cb_t cb = s_on_confirm;
    void* ud = s_user_data;
    confirm_close();
    if (cb)
        cb(ud);
}

void ui_confirm_show(const char* text, const char* confirm_label, ui_confirm_cb_t on_confirm,
                     void* user_data) {
    if (!text || !confirm_label)
        return;

    confirm_close();
    s_on_confirm = on_confirm;
    s_user_data = user_data;

    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(s_overlay, BR_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* panel = lv_obj_create(s_overlay);
    lv_obj_set_size(panel, 260, LV_SIZE_CONTENT);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, BR_COLOR_SURFACE, 0);
    lv_obj_set_style_border_color(panel, BR_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, BR_RADIUS, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_style_pad_row(panel, 10, 0);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* msg = lv_label_create(panel);
    lv_label_set_text(msg, text);
    lv_obj_set_style_text_font(msg, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(msg, BR_COLOR_TEXT, 0);
    lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(msg, LV_PCT(100));

    lv_obj_t* btn_row = lv_obj_create(panel);
    lv_obj_set_size(btn_row, LV_PCT(100), 36);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_group_t* g = lv_group_get_default();

    lv_obj_t* cancel_btn = lv_btn_create(btn_row);
    lv_obj_set_size(cancel_btn, 110, 32);
    lv_obj_align(cancel_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(cancel_btn, BR_COLOR_SURFACE_2, 0);
    lv_obj_add_event_cb(cancel_btn, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(cancel_lbl, "Cancel");
    lv_obj_set_style_text_font(cancel_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(cancel_lbl);

    lv_obj_t* ok_btn = lv_btn_create(btn_row);
    lv_obj_set_size(ok_btn, 110, 32);
    lv_obj_align(ok_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(ok_btn, BR_COLOR_DANGER, 0);
    lv_obj_add_event_cb(ok_btn, confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t* ok_lbl = lv_label_create(ok_btn);
    lv_label_set_text(ok_lbl, confirm_label);
    lv_obj_set_style_text_font(ok_lbl, &lv_font_montserrat_12, 0);
    lv_obj_center(ok_lbl);

    if (g) {
        lv_group_add_obj(g, cancel_btn);
        lv_group_add_obj(g, ok_btn);
        lv_group_focus_obj(cancel_btn);
    }
}
```

- [ ] **Step 3: Build gate + commit**

```bash
bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -1
git add components/ui_graphics
git commit -m "feat(ui_gfx): confirm dialog helper"
```

---

### Task 2: Confirmations for destructive actions

**Files:**
- Modify: `components/ui_graphics/screens/scr_settings.c` (`reboot_cb`, `conn_apply_cb`, `location_panic_off_cb`, `channel_remove_cb`; add `#include "ui_confirm.h"` and `#include "ui_toast.h"`)

- [ ] **Step 1: Reboot**

```c
static void do_reboot(void* user_data) {
    (void)user_data;
    ESP_LOGW(TAG, "Rebooting by user request...");
    esp_restart();
}

static void reboot_cb(lv_event_t* e) {
    (void)e;
    ui_confirm_show("Reboot the device?", "Reboot", do_reboot, NULL);
}
```

- [ ] **Step 2: Apply Mode & Reboot** (replace `conn_apply_cb`)

```c
static void do_apply_conn_mode(void* user_data) {
    (void)user_data;
    if (!s_conn_dropdown)
        return;
    conn_mode_t new_mode = (conn_mode_t)lv_dropdown_get_selected(s_conn_dropdown);
    conn_mode_set(new_mode);
    ESP_LOGW(TAG, "Connectivity mode set to %d, rebooting...", (int)new_mode);
    esp_restart();
}

static void conn_apply_cb(lv_event_t* e) {
    (void)e;
    if (!s_conn_dropdown)
        return;

    conn_mode_t new_mode = (conn_mode_t)lv_dropdown_get_selected(s_conn_dropdown);
    if (new_mode == conn_mode_get()) {
        ui_toast_show("Mode unchanged");
        return;
    }
    ui_confirm_show("Switch connectivity mode and reboot?", "Apply", do_apply_conn_mode, NULL);
}
```

- [ ] **Step 3: Panic Off** (replace `location_panic_off_cb`)

```c
static void do_panic_off(void* user_data) {
    (void)user_data;
    location_ui_apply_action(&s_loc_state, LOCATION_UI_ACTION_PANIC_OFF, 0);
    location_ui_save_state(&s_loc_state);
    if (s_loc_share_sw)
        lv_obj_clear_state(s_loc_share_sw, LV_STATE_CHECKED);
    ui_toast_show("Location sharing off");
}

static void location_panic_off_cb(lv_event_t* e) {
    (void)e;
    ui_confirm_show("Turn off all location sharing?", "Panic Off", do_panic_off, NULL);
}
```

- [ ] **Step 4: Channel remove** (replace `channel_remove_cb`)

```c
static void do_remove_channel(void* user_data) {
    int index = (int)(intptr_t)user_data;
    int rc = mesh_remove_channel(index);
    if (rc == 0) {
        ESP_LOGI(TAG, "Channel %d removed", index);
        ui_toast_show("Channel removed");
    } else {
        ESP_LOGE(TAG, "Failed to remove channel %d", index);
        ui_toast_show("Remove failed");
    }
    channel_refresh_list();
}

static void channel_remove_cb(lv_event_t* e) {
    int index = (int)(intptr_t)lv_event_get_user_data(e);
    ui_confirm_show("Remove this channel?", "Remove", do_remove_channel, (void*)(intptr_t)index);
}
```

(`do_remove_channel` needs a forward declaration of `channel_refresh_list`, which already exists at its use site; place these where `channel_remove_cb` currently lives.)

- [ ] **Step 5: Build gate + commit**

```bash
bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -1
git add components/ui_graphics
git commit -m "feat(ui_gfx): confirm destructive settings actions

Reboot, mode-switch reboot, location panic-off, and channel removal
now ask before firing; an unchanged mode apply says so instead of
silently doing nothing."
```

---

### Task 3: Feedback for mutating actions

**Files:**
- Modify: `components/ui_graphics/screens/scr_settings.c` (`radio_save_and_apply`, `channel_add_save_cb`, `channel_set_default_cb`, `name_edit_save_cb`)

- [ ] **Step 1: Radio apply** in `radio_save_and_apply`: after `ESP_LOGE(TAG, "radio_reconfigure failed");` add `ui_toast_show("Radio apply failed");` (before the return); after the final `ESP_LOGI(... "Radio config saved ...")` add `ui_toast_show("Radio settings applied");`

- [ ] **Step 2: Channel add** in `channel_add_save_cb`: replace

```c
    if (!name || name[0] == '\0')
        return;
```

with

```c
    if (!name || name[0] == '\0') {
        ui_toast_show("Channel name required");
        return; /* keep the modal open for correction */
    }
```

and in the result branches add `ui_toast_show("Channel created");` on success, `ui_toast_show("Failed to add channel");` on failure.

- [ ] **Step 3: Set default** in `channel_set_default_cb`: add `ui_toast_show("Default channel set");` after the `ESP_LOGI`.

- [ ] **Step 4: Name save** in `name_edit_save_cb`: give all three paths a voice:

```c
    if (new_name && new_name[0] != '\0') {
        if (mesh_set_node_name_persist(new_name) == 0) {
            if (s_name_label)
                lv_label_set_text(s_name_label, new_name);
            ESP_LOGI(TAG, "Node name updated to: %s", new_name);
            ui_toast_show("Name saved");
        } else {
            ESP_LOGW(TAG, "Failed to persist node name");
            ui_toast_show("Save failed");
        }
        name_edit_close();
    } else {
        ui_toast_show("Name required"); /* keep the modal open */
    }
```

(replace the unconditional `name_edit_close()` at the end with the structure above).

- [ ] **Step 5: Build gate + commit**

```bash
bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -1
git add components/ui_graphics
git commit -m "feat(ui_gfx): visible feedback for settings actions

Radio apply, channel add/remove/default, and node-name save now toast
their outcome; empty required fields say so instead of silently
discarding input."
```

---

### Task 4: Honest status bar (name, clock, GPS)

**Files:**
- Modify: `main/mesh_task.h` (declare getter next to `mesh_get_node_name`)
- Modify: `main/mesh_task.c` (implement getter following `mesh_get_node_name`'s locking pattern)
- Modify: `components/ui_graphics/screens/scr_layout.c` (`layout_update_status`)

**Interfaces:**
- Produces: `bool mesh_get_network_time_ms(int64_t* out_ms);` true and writes network epoch ms only when timesync is `synchronized`.

- [ ] **Step 1: Mesh getter**

`main/mesh_task.h`, after `const char* mesh_get_node_name(void);`:

```c
/* Network wall clock for display. Returns true and writes epoch ms only
 * when mesh timesync is synchronized. */
bool mesh_get_network_time_ms(int64_t* out_ms);
```

`main/mesh_task.c`, next to `mesh_get_node_name` (mirror its locking; if it takes no mutex, this racy read is display-only and fine):

```c
bool mesh_get_network_time_ms(int64_t* out_ms) {
    if (!out_ms || !s_timesync.synchronized)
        return false;
    *out_ms = timesync_get_network_time(&s_timesync, now_ms());
    return true;
}
```

- [ ] **Step 2: Status bar** in `components/ui_graphics/screens/scr_layout.c`:

Add includes/externs near the top:

```c
#include "board_config.h"
#include "gps.h"
#include <time.h>
```

```c
extern const char* mesh_get_node_name(void);
extern bool mesh_get_network_time_ms(int64_t* out_ms);
```

In `layout_update_status`, replace the trailing block:

```c
    /* Node name - keeping static "BRAMBLE" for now */
    /* identity component doesn't provide a get_name() function yet */
```

with:

```c
    /* Clock: network time when timesync has converged (UTC) */
    int64_t net_ms;
    if (mesh_get_network_time_ms(&net_ms)) {
        time_t t = (time_t)(net_ms / 1000);
        struct tm tm_utc;
        gmtime_r(&t, &tm_utc);
        snprintf(buf, sizeof(buf), "%02d:%02d", tm_utc.tm_hour, tm_utc.tm_min);
        lv_label_set_text(layout->lbl_time, buf);
    } else {
        lv_label_set_text(layout->lbl_time, "--:--");
    }

    /* GPS: live fix state; hidden entirely on GPS-less boards */
    if (board_has_cap(BOARD_CAP_GPS)) {
        lv_obj_clear_flag(layout->lbl_gps, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_text_color(layout->lbl_gps,
                                    gps_has_fix() ? BR_COLOR_PRIMARY : BR_COLOR_TEXT_SEC, 0);
    } else {
        lv_obj_add_flag(layout->lbl_gps, LV_OBJ_FLAG_HIDDEN);
    }

    /* Node name from the mesh (settable via Settings / bramble.setNodeName) */
    const char* name = mesh_get_node_name();
    lv_label_set_text(layout->lbl_node_name, (name && name[0]) ? name : "BRAMBLE");
```

- [ ] **Step 3: Build gates + commit**

```bash
bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -1
bash scripts/flash.sh local heltec-v3 build 2>&1 | tail -1
git add main components/ui_graphics
git commit -m "feat(ui_gfx): honest status bar

Node name comes from the mesh instead of a hardcoded BRAMBLE, the
clock shows network time (UTC) once timesync converges instead of a
permanent placeholder, and the GPS label reflects live fix state
(hidden on GPS-less boards). Adds mesh_get_network_time_ms()."
```

---

### Task 5: Version truth + chat title/switcher fixes

**Files:**
- Modify: `components/ui_graphics/screens/scr_settings.c` (version row)
- Modify: `components/ui_graphics/screens/scr_chat_messages.c` (`update_title`, target button)

- [ ] **Step 1: Version from the app descriptor**

In `scr_settings.c` add `#include "esp_app_desc.h"`, and replace

```c
    lv_label_set_text(ver_val, "0.9.1-tdeck");
```

with

```c
    lv_label_set_text(ver_val, esp_app_get_description()->version);
```

- [ ] **Step 2: DM title falls back to the address, not "Chat"**

In `update_title` (`scr_chat_messages.c`), replace the format block:

```c
    static char buf[48];
    if (s_target.kind == CHAT_TARGET_DM) {
        if (peer_name && peer_name[0]) {
            snprintf(buf, sizeof(buf), "%s", peer_name);
        } else {
            snprintf(buf, sizeof(buf), "%08lX", (unsigned long)s_target.peer_addr);
        }
    } else if (channel_name) {
        snprintf(buf, sizeof(buf), "#%s", channel_name);
    } else {
        snprintf(buf, sizeof(buf), "Chat");
    }
    lv_label_set_text(s_title, buf);
```

- [ ] **Step 3: Make the target switcher visible**

In `open_with_target`, after the `target_btn` styling line (`lv_obj_set_style_bg_color(target_btn, BR_COLOR_SURFACE, 0);`), add:

```c
    lv_obj_set_style_border_color(target_btn, BR_COLOR_BORDER, 0);
    lv_obj_set_style_border_width(target_btn, 1, 0);
    lv_obj_t* tgt_lbl = lv_label_create(target_btn);
    lv_label_set_text(tgt_lbl, LV_SYMBOL_REFRESH " channel");
    lv_obj_set_style_text_font(tgt_lbl, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(tgt_lbl, BR_COLOR_TEXT_SEC, 0);
    lv_obj_center(tgt_lbl);
```

- [ ] **Step 4: Build gate + commit**

```bash
bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -1
git add components/ui_graphics
git commit -m "fix(ui_gfx): truthful version label, DM titles, visible channel switcher

The Settings version row now reads the app descriptor instead of a
hardcoded string, unnamed DM peers title the conversation with their
address instead of Chat, and the conversation target switcher gets a
label and border instead of being an invisible button."
```

---

### Task 6: Full gate + PR

- [ ] **Step 1: Gate**

```bash
bash test/run_all_tests.sh 2>&1 | tail -2
bash scripts/flash.sh local tdeck-plus build 2>&1 | tail -1
bash scripts/flash.sh local heltec-v3 build 2>&1 | tail -1
bash scripts/flash.sh local heltec-v4 build 2>&1 | tail -1
```

- [ ] **Step 2: clang-format changed files, commit any delta as `style: clang-format the trust batch`.**

- [ ] **Step 3: Commit this plan file, push, open PR via Gitea API** (title `feat(ui_gfx): T-Deck trust batch`, body = findings fixed + test plan). Poll CI. Report URL and STOP; merge only on user instruction.
