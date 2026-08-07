/*
 * nRF-only extension of components/battery/include/battery.h, implemented by
 * every nrf/shim battery backend (battery_t1000e.c for real hardware,
 * battery_null.c as a no-op).
 *
 * Why this exists: on the T1000-E a voltage read must energize the P1.06
 * sensor rail, and the one boot that ever drove that pin from boot context
 * stopped the board dead (reset or lockup, undetermined), while the same
 * drive is bench-proven safe at runtime. The mesh task's FIRST beacon fires
 * immediately at mesh start (main/mesh_task.c, the boot-stage send_beacon
 * before the loop), i.e. inside the boot window, and it reads the battery
 * (main/mesh_beacon.c). This hook lets app_init hold the rail-touching path
 * disarmed through that window: an un-armed read returns 0 without touching
 * any hardware, and app_init arms the backend only after BT_BOOT_DONE, so
 * the first real gated window always runs in the runtime context the bench
 * probe proved safe.
 */
#pragma once

#include <stdint.h>

/* Allow rail-gated voltage reads from now on. Called once by app_init after
 * BT_BOOT_DONE. Before this call battery_read_mv() returns 0 and touches no
 * hardware. */
void battery_runtime_arm(void);

/* Persisted survival-latch verdict, for the boot trace (aux of
 * BT_BATTERY_INIT): 0 = first gated window not yet attempted, 1 = a
 * previous attempt never completed (voltage path disabled this boot),
 * 2 = a past window completed, the drive is proven on this unit. */
uint8_t battery_probe_state(void);
