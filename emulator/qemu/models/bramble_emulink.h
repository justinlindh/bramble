/*
 * Bramble emu-link bridge (Phase 2 emulator): the QEMU pager's JSON-over-socket
 * link to the gosim ether. See hw/xtensa/bramble/bramble_emulink.c.
 */

#ifndef HW_XTENSA_BRAMBLE_EMULINK_H
#define HW_XTENSA_BRAMBLE_EMULINK_H

#include "qapi/qmp/qdict.h"

/* Handler for an inbound emu-link message of a registered "t" type. */
typedef void (*emulink_handler_t)(QDict *msg, void *ctx);

/*
 * Register (or replace) the handler for inbound messages of type `type`. The
 * handler table is static and independent of chardev attach order, so a device
 * model may register at realize even though the chardev is wired later at
 * machine init. Returns 0 on success, -1 on a bad type or a full table.
 */
int emulink_on(const char *type, emulink_handler_t fn, void *ctx);

/*
 * Write an already-framed line (message text including its trailing '\n') to the
 * emu-link socket. The message builders below use this; the indicator bridge
 * uses it directly for its `ind` line. A no-op returning -1 if the link is not
 * connected, so a standalone boot is unaffected.
 */
int emulink_write(const char *s, size_t len);

/* Emit a `tx`: PHY bytes base64-encoded, plus the latched modulation params
 * (freq MHz, sf, bw Hz, cr, power dBm). No-op if the link is not connected. */
int emulink_send_tx(const uint8_t *payload, unsigned len, int freq_mhz,
                    int sf, int bw_hz, int cr, int power);

/* Emit an `fb`: the resolved 1bpp logical framebuffer base64-encoded, plus the
 * refresh kind ("full"/"partial") and busy duration, matching display_virt.c's
 * message shape. No-op if the link is not connected. */
int emulink_send_fb(const uint8_t *fb, size_t fb_len, uint32_t seq,
                    const char *kind, uint32_t busy_ms);

/*
 * Wire the emu-link bridge to the chardev named "emulink" (the gosim supervisor
 * adds it with `-chardev socket,id=emulink,path=<broker>`), so the SX1262 model
 * exchanges LoRa frames with the gosim ether and the QEMU pager meshes with the
 * linux pagers. A no-op if no such chardev exists (standalone run-qemu.sh boot).
 * Called once from bramble_attach().
 */
void bramble_emulink_attach(void);

#endif
