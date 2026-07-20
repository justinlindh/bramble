/*
 * SSD1680 e-paper engine (pure C, host-testable).
 *
 * Command sequences follow the GDEY0213B74 datasheet (Dalian Good Display,
 * 2.13 inch series, SSD1680Z8 controller), section 14 "Typical Operating
 * Sequence" (p.31), with register values cross-checked against the panel
 * vendor reference flow (same values as the GxEPD2_213_GDEY0213B74 /
 * Waveshare 2.13 V4 community drivers for this exact panel). Timing and
 * refresh-cadence seeds come from the p.10 DC characteristics table. See
 * ssd1680_engine.h for the geometry and layout contract.
 *
 * No ESP-IDF includes here: this file compiles in the plain-gcc test
 * harness (test/test_ssd1680_engine.c) and on both device and emulator
 * targets unchanged.
 */
#include "ssd1680_engine.h"

#include <string.h>

/* ── state ───────────────────────────────────────────────────────────── */

/* Logical landscape framebuffer: bit set = black ink. */
static uint8_t s_fb[SSD1680_FB_SIZE];

/* Native controller RAM stream rebuilt at each flush: 1 = white. */
static uint8_t s_ram[SSD1680_RAM_SIZE];

static bool s_dirty;         /* any real pixel change since last refresh */
static uint32_t s_refreshes; /* emitted refreshes; 0 = first still pending */
static bool s_force_full;    /* caller-requested full refresh pending */

/* Snapshot of the fb at the last FULL refresh: the ghost baseline. Ghosting
 * accumulates from every partial since the last full clear, so the
 * change-fraction heuristic measures cumulative divergence from this
 * image, not from the previous flush. */
static uint8_t s_shown[SSD1680_FB_SIZE];

/* Op scratch: longest stream is the full refresh (15 ops). */
static ssd1680_op_t s_ops[16];

/* ── fixed command payloads (datasheet p.31 sequence, values per panel
 *    vendor reference flow) ───────────────────────────────────────────── */

/* 0x01 driver output control: MUX = 249 -> 250 gate lines (p.6 resolution
 * 122(H) x 250(V)); GD = SM = TB = 0 (default scan order). */
static const uint8_t d_driver_output[] = {0xF9, 0x00, 0x00};

/* 0x11 data entry mode: X increment, Y increment, counter advances along
 * X. The RAM stream below is emitted in exactly this order. */
static const uint8_t d_data_entry[] = {0x03};

/* 0x44 RAM X window: X is addressed in BYTES (8 sources each). 122
 * sources -> bytes 0x00..0x0F (16 bytes = 128 slots; the last 6 source
 * slots per row are pad, kept white). */
static const uint8_t d_x_window[] = {0x00, 0x0F};

/* 0x45 RAM Y window: gates 0..249, 16-bit little-endian pairs. */
static const uint8_t d_y_window[] = {0x00, 0x00, 0xF9, 0x00};

/* 0x3C border waveform: 0x05 (follow LUT) for full refreshes; 0x80 (VCOM)
 * for partials so the border does not flicker on every update. */
static const uint8_t d_border_full[] = {0x05};
static const uint8_t d_border_partial[] = {0x80};

/* 0x21 display update control 1: normal RAM, source output S8..S167. */
static const uint8_t d_update_ctrl1[] = {0x00, 0x80};

/* 0x18 temperature sensor: 0x80 = internal sensor (p.31 step 4). */
static const uint8_t d_temp_internal[] = {0x80};

/* 0x4E / 0x4F RAM address counters: reset to the window origin. */
static const uint8_t d_x_counter[] = {0x00};
static const uint8_t d_y_counter[] = {0x00, 0x00};

/* 0x22 display update control 2 sequence options (p.31 steps 4+5 "Load
 * waveform LUT / Drive display panel by Command 0x22, 0x20"):
 *   0xF7 = enable clock+analog, load temperature, Display Mode 1 (full),
 *          then disable analog+clock.
 *   0xFF = same with Display Mode 2 (differential partial; the controller
 *          ping-pongs 0x24 into the previous-image RAM afterwards). */
static const uint8_t d_duc2_full[] = {0xF7};
static const uint8_t d_duc2_partial[] = {0xFF};

/* 0x10 deep sleep mode 1: retains RAM (the Mode 2 diff base must survive
 * between flushes); ~1 uA (p.10). Exit requires HW reset, which the io
 * layer performs at the start of every flush. */
static const uint8_t d_deep_sleep[] = {0x01};

/* ── framebuffer ─────────────────────────────────────────────────────── */

void ssd1680_engine_init(void) {
    memset(s_fb, 0, sizeof(s_fb));
    s_dirty = false;
    s_refreshes = 0; /* forces the first flush to emit a FULL refresh */
    s_force_full = false;
    memset(s_shown, 0, sizeof(s_shown));
}

void ssd1680_engine_request_full_refresh(void) { s_force_full = true; }

void ssd1680_engine_pixel(int x, int y, bool on) {
    if (x < 0 || x >= SSD1680_WIDTH || y < 0 || y >= SSD1680_HEIGHT)
        return;
    uint8_t* byte = &s_fb[y * SSD1680_FB_STRIDE + x / 8];
    uint8_t mask = (uint8_t)(0x80u >> (x & 7)); /* MSB = leftmost pixel */
    uint8_t before = *byte;
    if (on)
        *byte |= mask;
    else
        *byte &= (uint8_t)~mask;
    if (*byte != before)
        s_dirty = true;
}

const uint8_t* ssd1680_engine_fb(void) { return s_fb; }

/* ── native RAM stream ───────────────────────────────────────────────── */

/*
 * Rotation mapping, the single place orientation is decided:
 *   logical (lx, ly) -> native source sx = 121 - ly, gate gy = lx.
 * The pager PCB's mirrored FPC connector is electrical only; it is NOT
 * compensated here (hardware doc rule). If bring-up shows a flipped
 * image, change these two expressions.
 */
#define SSD1680_MAP_LX(gy) (gy)
#define SSD1680_MAP_LY(sx) (SSD1680_HEIGHT - 1 - (sx))

static void build_ram_stream(void) {
    /* Emitted in data-entry-mode 0x03 order: X bytes ascending within a
     * gate row, gate rows ascending. RAM polarity 1 = white, so a set
     * logical bit (black ink) clears the RAM bit. Pad sources 122..127
     * (low 6 bits of each row's byte 15) are written white. */
    for (int gy = 0; gy < SSD1680_WIDTH; gy++) {
        int lx = SSD1680_MAP_LX(gy);
        const uint8_t* lrow_base = s_fb; /* indexed per pixel below */
        uint8_t* out = &s_ram[gy * SSD1680_RAM_STRIDE];
        for (int xb = 0; xb < SSD1680_RAM_STRIDE; xb++) {
            uint8_t b = 0xFF; /* white, covers the pad bits too */
            for (int bit = 0; bit < 8; bit++) {
                int sx = xb * 8 + bit; /* MSB = lowest source address */
                if (sx >= SSD1680_HEIGHT)
                    break; /* pad region: stays white */
                int ly = SSD1680_MAP_LY(sx);
                uint8_t lmask = (uint8_t)(0x80u >> (lx & 7));
                if (lrow_base[ly * SSD1680_FB_STRIDE + lx / 8] & lmask)
                    b &= (uint8_t) ~(0x80u >> bit); /* black */
            }
            out[xb] = b;
        }
    }
}

/* ── flush / command stream ──────────────────────────────────────────── */

static size_t emit(size_t i, uint8_t cmd, const uint8_t* data, size_t len) {
    s_ops[i].cmd = cmd;
    s_ops[i].data = data;
    s_ops[i].len = len;
    return i + 1;
}

ssd1680_refresh_t ssd1680_engine_flush(const ssd1680_op_t** ops, size_t* n_ops, uint32_t* busy_ms) {
    bool first = (s_refreshes == 0);
    bool forced = s_force_full;
    if (!first && !s_dirty && !forced) {
        *ops = NULL;
        *n_ops = 0;
        *busy_ms = 0;
        return SSD1680_REFRESH_NONE;
    }

    bool full = forced || (s_refreshes % SSD1680_FULL_EVERY_N_FLUSHES) == 0;

    /* Change-fraction promotion: when a large share of the framebuffer
     * diverges from the last FULL image (a message scroll or screen/menu
     * change in one flush, or many partials drifting far cumulatively), a
     * partial would carry heavy ghosting across the whole area, so promote
     * to a full refresh now. Small in-place updates stay partial. */
    if (!full) {
        size_t changed = 0;
        for (size_t b = 0; b < SSD1680_FB_SIZE; b++) {
            if (s_fb[b] != s_shown[b])
                changed++;
        }
        if (changed * 100 >= (size_t)SSD1680_FULL_CHANGE_PCT * SSD1680_FB_SIZE)
            full = true;
    }

    build_ram_stream();

    /* GDEY0213B74 p.31 typical operating sequence. The io layer HW-resets
     * (wakes from deep sleep) before replaying, and waits BUSY low after
     * 0x12 and 0x20. */
    size_t i = 0;
    if (full)
        i = emit(i, SSD1680_CMD_SW_RESET, NULL, 0);
    i = emit(i, SSD1680_CMD_DRIVER_OUTPUT, d_driver_output, sizeof(d_driver_output));
    i = emit(i, SSD1680_CMD_DATA_ENTRY, d_data_entry, sizeof(d_data_entry));
    i = emit(i, SSD1680_CMD_RAM_X_WINDOW, d_x_window, sizeof(d_x_window));
    i = emit(i, SSD1680_CMD_RAM_Y_WINDOW, d_y_window, sizeof(d_y_window));
    i = emit(i, SSD1680_CMD_BORDER, full ? d_border_full : d_border_partial, 1);
    i = emit(i, SSD1680_CMD_UPDATE_CTRL1, d_update_ctrl1, sizeof(d_update_ctrl1));
    i = emit(i, SSD1680_CMD_TEMP_SENSOR, d_temp_internal, sizeof(d_temp_internal));
    i = emit(i, SSD1680_CMD_RAM_X_COUNTER, d_x_counter, sizeof(d_x_counter));
    i = emit(i, SSD1680_CMD_RAM_Y_COUNTER, d_y_counter, sizeof(d_y_counter));
    i = emit(i, SSD1680_CMD_WRITE_RAM_BW, s_ram, SSD1680_RAM_SIZE);
    if (full) {
        /* Seed the previous-image RAM so Mode 2 partials diff against a
         * known base; partials rely on the controller's automatic
         * ping-pong afterwards. */
        i = emit(i, SSD1680_CMD_WRITE_RAM_RED, s_ram, SSD1680_RAM_SIZE);
        i = emit(i, SSD1680_CMD_UPDATE_CTRL2, d_duc2_full, sizeof(d_duc2_full));
    } else {
        i = emit(i, SSD1680_CMD_UPDATE_CTRL2, d_duc2_partial, sizeof(d_duc2_partial));
    }
    i = emit(i, SSD1680_CMD_MASTER_ACTIVATE, NULL, 0);
    i = emit(i, SSD1680_CMD_DEEP_SLEEP, d_deep_sleep, sizeof(d_deep_sleep));

    *ops = s_ops;
    *n_ops = i;
    *busy_ms = full ? SSD1680_BUSY_MS_FULL : SSD1680_BUSY_MS_PARTIAL;

    s_refreshes++;
    s_dirty = false;
    s_force_full = false;
    if (full)
        memcpy(s_shown, s_fb, SSD1680_FB_SIZE); /* new ghost baseline */
    return full ? SSD1680_REFRESH_FULL : SSD1680_REFRESH_PARTIAL;
}
