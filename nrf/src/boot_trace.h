/*
 * Flash-backed boot trace for consoleless boards (T1000-E).
 *
 * The board has no UART and BLE is the thing whose bring-up we are trying
 * to observe, so the only channel that survives every failure mode is
 * internal flash: each boot stage writes a (tag, aux) word pair into a
 * reserved page, and on a fatal condition the device stamps a failure tag
 * and reboots into the UF2 bootloader, whose CURRENT.UF2 dump makes the
 * page readable from the host with no debugger attached.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Last 4KB page of the app flash region (app link region ends at 0xC0000
 * where littlefs begins; the size gate caps the image far below this). */
#define BOOT_TRACE_PAGE 0x000BF000u
#define BOOT_TRACE_MAGIC 0x42545243u /* "BTRC" */

/* Stage tags (aux meaning in parentheses). */
#define BT_MAIN_ENTRY 0x01u    /* (0) after vector/NVIC hygiene */
#define BT_CRYPTO_CHECK 0x02u  /* (0) entering crypto self-check */
#define BT_CRYPTO_OK 0x03u     /* (0) */
#define BT_NVS_INIT 0x04u      /* (rc) nvs_flash_init result */
#define BT_IDENTITY 0x05u      /* (rc) load/generate result */
#define BT_IDENTITY_ADDR 0x06u /* (addr) node address */
#define BT_MSG_STORE 0x07u     /* (0) */
#define BT_RPC_READY 0x08u     /* (0) */
#define BT_TOKEN_SEED 0x09u    /* (rc) */
#define BT_TOKEN_LOADED 0x0Au  /* (0) */
#define BT_GPS_INIT 0x0Bu      /* (rc) gps_init result, GNSS boards only */
#define BT_HFXO_OK 0x0Cu       /* (0) high-frequency crystal running */
#define BT_LFCLK 0x0Du         /* (1 xtal / 0 rc) */
#define BT_BLE_INIT 0x0Eu      /* (rc) ble_server_init */
#define BT_BLE_START 0x0Fu     /* (rc) ble_server_start */
#define BT_ADV 0x10u           /* (rc) ble_gap_adv_start result */
#define BT_MESH_STARTED 0x11u  /* (0) */
#define BT_BATTERY_INIT 0x12u  /* (0) battery_init complete */
#define BT_BOOT_DONE 0xDDu     /* (free heap) app_init complete */

/* Failure tags: boot_trace_fail stamps these and reboots into DFU. */
#define BT_FAIL_ASSERT 0xE1u    /* (line) */
#define BT_FAIL_STACK_OVF 0xE2u /* (0) */
#define BT_FAIL_MALLOC 0xE3u    /* (0) */
#define BT_FAIL_SENTINEL 0xE4u  /* (last stage tag seen) */
#define BT_FAIL_NRFX 0xE5u      /* (nrfx source line) NRFX_ASSERT failed */
#define BT_FAIL_HARDFAULT 0xEFu /* (stacked PC) */

/* Erases the trace page and stamps the magic. Call once, early, before the
 * scheduler starts (the blocking page erase is ~85ms). */
void boot_trace_init(void);

/* Appends one (tag, aux) pair. Safe from any context including faults. */
void boot_trace_mark(uint32_t tag, uint32_t aux);

/* Stamps a failure pair, then reboots into the UF2 bootloader by writing
 * the Adafruit DFU magic to GPREGRET. From DFU the host reads the trace
 * back out of CURRENT.UF2. Never returns. */
void boot_trace_fail(uint32_t tag, uint32_t aux);

/* Most recent stage tag, and whether advertising came up (BT_ADV rc==0). */
uint32_t boot_trace_last(void);
bool boot_trace_adv_ok(void);

/* NRFX_ASSERT's failure handler (see nrf/config/nrfx_glue.h, which declares
 * it independently so no nrfx translation unit has to include this header).
 * Stamps BT_FAIL_NRFX with the asserting nrfx source line and reboots.
 *
 * `line` alone does not name the file. If this tag ever appears, grep the
 * vendored tree for an NRFX_ASSERT on exactly that line to narrow it:
 *   grep -n "NRFX_ASSERT" nrf/build-t1000e/_deps/nrfx-src/drivers/src/*.c \
 *     | awk -F: '$2 == LINE'
 * Only a handful of files carry an assert on any given line, and the last
 * stage tags in the trace say which peripheral was live. */
void bramble_nrfx_assert_failed(uint32_t line) __attribute__((noreturn));
