#ifndef BRAMBLE_SSD1680_ENGINE_H
#define BRAMBLE_SSD1680_ENGINE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/*
 * SSD1680 e-paper engine for the GDEY0213B74 2.13" panel (Bramble Pager
 * v1). Pure C, no ESP-IDF includes: compiles in the plain-gcc host test
 * harness. The engine owns the logical framebuffer, dirty tracking, the
 * partial/full refresh policy and SSD1680 command-stream generation.
 *
 * Consumers:
 *   - ssd1680_io.c (device): replays flush ops verbatim over 4-wire SPI.
 *   - display_virt.c (emulator): ships the logical fb + refresh kind +
 *     busy_ms to the emu-link broker; ignores the raw op stream.
 *
 * Geometry. The panel is physically 122 source lines x 250 gate lines
 * (GDEY0213B74 datasheet p.6: 122(H) x 250(V)); the firmware sees it in
 * landscape as 250 wide x 122 high. Two layouts exist:
 *
 *   Logical fb (ssd1680_engine_fb): 250x122 1bpp, row-major, 32 bytes per
 *   row (250 px -> 31.25 bytes, rounded up), 3904 bytes. MSB of each byte
 *   is the leftmost pixel of its 8-pixel group; bit set = black ink. The
 *   6 pad bits per row are the LOW bits of the row's LAST byte (byte 31,
 *   logical x 250..255) and are never set.
 *
 *   Controller RAM stream (inside 0x24/0x26 flush ops): portrait-native,
 *   250 gate rows x 16 X-bytes = 4000 bytes. The SSD1680 addresses X in
 *   bytes of 8 source pixels; 122 sources round up to 16 bytes, with the
 *   6 pad bits in the LOW bits of each row's byte 15 (sources 122..127),
 *   written white. RAM polarity: 1 = white, 0 = black. Rotation mapping:
 *   logical (lx, ly) -> source 121 - ly, gate lx (see SSD1680_MAP_* in
 *   ssd1680_engine.c; the pager PCB's mirrored connector is electrical
 *   only and is deliberately NOT compensated here).
 */

#define SSD1680_WIDTH 250
#define SSD1680_HEIGHT 122

/* Logical framebuffer: what engine_fb() returns and emu-link "fb" carries. */
#define SSD1680_FB_STRIDE 32
#define SSD1680_FB_SIZE (SSD1680_FB_STRIDE * SSD1680_HEIGHT) /* 3904 */

/* Native controller RAM stream: payload of the 0x24/0x26 ops. */
#define SSD1680_RAM_STRIDE 16
#define SSD1680_RAM_SIZE (SSD1680_RAM_STRIDE * SSD1680_WIDTH) /* 4000 */

/*
 * Refresh policy and timing table: the ONE place bring-up tunes.
 *   - Full refresh cadence: GDEY0213B74 p.10 recommends a full refresh
 *     after 5 consecutive partials to limit ghosting; the emulator design
 *     starts at 10 (hardware doc suggests trying 5 at bring-up).
 *   - busy_ms seeds: p.10 DC characteristics, full update 3 s typ,
 *     partial update 0.42 s typ (rounded up to 500 ms).
 */
#define SSD1680_FULL_EVERY_N_FLUSHES 10
#define SSD1680_BUSY_MS_PARTIAL 500u
#define SSD1680_BUSY_MS_FULL 3000u
/* Promote a flush to FULL when at least this percentage of framebuffer
 * bytes changed since the last emitted refresh. Catches the ghost-heavy
 * cases (message scroll, screen/menu change) the moment they happen
 * instead of waiting for the every-N cadence; small in-place updates
 * (badge, appended line) stay partial. */
#define SSD1680_FULL_CHANGE_PCT 20

/* SSD1680 command vocabulary used by the engine (GDEY0213B74 p.31,
 * typical operating sequence). */
#define SSD1680_CMD_DRIVER_OUTPUT 0x01   /* gate driver output control */
#define SSD1680_CMD_DEEP_SLEEP 0x10      /* deep sleep mode */
#define SSD1680_CMD_DATA_ENTRY 0x11      /* data entry mode */
#define SSD1680_CMD_SW_RESET 0x12        /* software reset */
#define SSD1680_CMD_TEMP_SENSOR 0x18     /* temperature sensor select */
#define SSD1680_CMD_MASTER_ACTIVATE 0x20 /* master activation */
#define SSD1680_CMD_UPDATE_CTRL1 0x21    /* display update control 1 */
#define SSD1680_CMD_UPDATE_CTRL2 0x22    /* display update control 2 */
#define SSD1680_CMD_WRITE_RAM_BW 0x24    /* write B/W image RAM */
#define SSD1680_CMD_WRITE_RAM_RED 0x26   /* write previous-image RAM */
#define SSD1680_CMD_BORDER 0x3C          /* border waveform control */
#define SSD1680_CMD_RAM_X_WINDOW 0x44    /* RAM X start/end (byte units) */
#define SSD1680_CMD_RAM_Y_WINDOW 0x45    /* RAM Y start/end */
#define SSD1680_CMD_RAM_X_COUNTER 0x4E   /* RAM X address counter */
#define SSD1680_CMD_RAM_Y_COUNTER 0x4F   /* RAM Y address counter */

typedef enum {
    SSD1680_REFRESH_NONE,
    SSD1680_REFRESH_PARTIAL,
    SSD1680_REFRESH_FULL
} ssd1680_refresh_t;

/* One SPI operation: command byte plus optional data bytes (D/C high).
 * data points into engine-owned storage, valid until the next flush. */
typedef struct {
    uint8_t cmd;
    const uint8_t* data;
    size_t len;
} ssd1680_op_t;

/* Reset all engine state: clears the framebuffer and forces the next
 * (first) refresh to be FULL regardless of dirty state, since the
 * physical panel may hold a stale image. */
void ssd1680_engine_init(void);

/*
 * Force the NEXT ssd1680_engine_flush() to be FULL, regardless of the
 * every-N cadence or the change-fraction heuristic. Also overrides the
 * "nothing changed" elision: a pending forced refresh emits even if no
 * pixel changed since the last flush, so a caller can use this to clear
 * accumulated ghosting on a semantic boundary (e.g. a screen change) that
 * a byte-diff heuristic would not reliably catch on a sparse, mostly-blank
 * text UI. Consumed by the next flush call; has no effect if a flush does
 * not follow (idempotent, safe to call more than once before the flush).
 */
void ssd1680_engine_request_full_refresh(void);

/* Set/clear one logical pixel (on = black ink). Out-of-range coordinates
 * are ignored. Marks the frame dirty only on a real bit change. */
void ssd1680_engine_pixel(int x, int y, bool on);

/*
 * Produce the command stream for one refresh. Returns the refresh kind:
 *   NONE:    nothing changed since the last emitted refresh (and this is
 *            not the first flush). *n_ops = 0, *busy_ms = 0.
 *   FULL:    first flush after init, then every SSD1680_FULL_EVERY_N_FLUSHES
 *            emitted refreshes. Display Mode 1, writes both RAM planes.
 *   PARTIAL: everything else. Display Mode 2 differential update.
 * The returned ops (and their data) live in engine-owned static storage
 * valid until the next flush call. NONE flushes do not advance the
 * full-refresh cadence.
 */
ssd1680_refresh_t ssd1680_engine_flush(const ssd1680_op_t** ops, size_t* n_ops, uint32_t* busy_ms);

/* Logical framebuffer, SSD1680_FB_SIZE bytes (see layout note above). */
const uint8_t* ssd1680_engine_fb(void);

#endif /* BRAMBLE_SSD1680_ENGINE_H */
