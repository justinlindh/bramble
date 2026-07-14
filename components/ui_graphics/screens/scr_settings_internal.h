#ifndef SCR_SETTINGS_INTERNAL_H
#define SCR_SETTINGS_INTERNAL_H

#include "lvgl.h"
#include "scr_layout.h"
#include <stddef.h>

/* Cross-page helpers shared by the Settings hub (scr_settings.c) and its
 * subpages (scr_settings_<domain>.c). The hub is a normal tab whose rows open a
 * subpage; every subpage rebuilds the content area through layout_rebuild_content
 * and returns to the hub via the chrome back chevron settings_subpage_begin
 * installs. */

/* A 304px settings row with a left-aligned label. The caller adds its value
 * label or control (right aligned) and, for a tappable row, registers the row
 * with the content group and a deferred click. Identical to the row every flat
 * settings section used before the hub split. */
lv_obj_t* settings_create_setting_row(lv_obj_t* parent, const char* label);

/* Begin a settings subpage: install the chrome header (a back chevron that
 * returns to the hub, plus the page title) and return the scrollable content
 * column the subpage hangs its rows on. Content widgets join
 * lv_group_get_default(); the returned column is ui_zone_scroll_column, so
 * scroll-on-focus reaches rows past the fold. Call once at the top of a builder. */
lv_obj_t* settings_subpage_begin(bramble_layout_t* layout, const char* title);

/* Subpage builders. Each runs through layout_rebuild_content (from a hub row's
 * deferred click, and re-run when the hub rebuilds on back-navigation). */
void settings_identity_builder(bramble_layout_t* layout, void* ctx);
void settings_device_builder(bramble_layout_t* layout, void* ctx);
void settings_location_builder(bramble_layout_t* layout, void* ctx);
void settings_channels_builder(bramble_layout_t* layout, void* ctx);
void settings_radio_builder(bramble_layout_t* layout, void* ctx);
void settings_connectivity_builder(bramble_layout_t* layout, void* ctx);

/* Live one-line summaries the hub renders next to each row. Each reads the same
 * source its subpage reads (NVS / driver state), so the hub value is current and
 * refreshes when the hub is rebuilt on back-navigation. Write an ASCII (or
 * LV_SYMBOL_*) string, never wider than the buffer. */
void settings_identity_summary(char* buf, size_t n);
void settings_device_summary(char* buf, size_t n);
void settings_location_summary(char* buf, size_t n);
void settings_channels_summary(char* buf, size_t n);
void settings_radio_summary(char* buf, size_t n);
void settings_connectivity_summary(char* buf, size_t n);

#endif /* SCR_SETTINGS_INTERNAL_H */
