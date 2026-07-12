/*
 * SSD1680 e-paper engine tests (Bramble Pager v1, GDEY0213B74 panel).
 *
 * Every command sequence asserted here is pinned to the GDEY0213B74
 * datasheet (Dalian Good Display, 2.13 inch series, SSD1680Z8 controller):
 *   - p.6  section 3: display resolution 122(H) x 250(V), i.e. 122 source
 *          lines by 250 gate lines. The firmware sees the panel in
 *          landscape: 250 wide x 122 high.
 *   - p.8  notes 5-1..5-5: 4-wire SPI (BS1 = L), D/C# high = data / low =
 *          command, RES# active low, BUSY active HIGH (command must not be
 *          sent while BUSY is high).
 *   - p.10 DC characteristics: full update 3 s typ, partial update 0.42 s
 *          typ; "add a full-screen refresh after 5 consecutive [partial]
 *          operations" guidance (we run 10, one tunable constant).
 *   - p.31 section 14, typical operating sequence: HW reset -> SW reset
 *          (0x12) -> gate driver output (0x01) -> RAM size (0x11, 0x44,
 *          0x45) -> border (0x3C) -> temperature (0x18) -> LUT via
 *          (0x22, 0x20) -> image RAM (0x4E, 0x4F, 0x24, 0x26) -> drive
 *          panel (0x22, 0x20) -> deep sleep (0x10).
 *
 * The engine is pure C (no IDF): it owns the logical framebuffer, dirty
 * tracking, refresh policy and command-stream generation. ssd1680_io.c
 * replays the returned ops verbatim over SPI on the real board.
 */
#include "unity.h"
#include "ssd1680_engine.h"

#include <string.h>

void setUp(void) { ssd1680_engine_init(); }
void tearDown(void) {}

/* ── helpers ─────────────────────────────────────────────────────────── */

static const ssd1680_op_t* find_op(const ssd1680_op_t* ops, size_t n, uint8_t cmd) {
    for (size_t i = 0; i < n; i++)
        if (ops[i].cmd == cmd)
            return &ops[i];
    return NULL;
}

static int op_index(const ssd1680_op_t* ops, size_t n, uint8_t cmd) {
    for (size_t i = 0; i < n; i++)
        if (ops[i].cmd == cmd)
            return (int)i;
    return -1;
}

static ssd1680_refresh_t flush(const ssd1680_op_t** ops, size_t* n, uint32_t* busy) {
    return ssd1680_engine_flush(ops, n, busy);
}

/* Dirty one pixel so the next flush is not elided as NONE. Uses a fresh
 * coordinate each call (engine fb starts all-clear per setUp, so setting a
 * never-touched pixel is always a real change). */
static void touch(void) {
    static int t = 0;
    t++;
    ssd1680_engine_pixel(t % 250, 100 + (t / 250) % 22, true);
}

/* ── logical framebuffer layout ──────────────────────────────────────── */

/*
 * Logical fb layout contract (what display_virt ships over emu-link and
 * what the frontend renders): 250x122 1bpp, row-major, 32 bytes per row,
 * 3904 bytes total. Bit order follows the SSD1680 RAM mapping convention
 * (GDEY0213B74 p.31 image RAM; SSD1680 RAM is MSB-first along the address
 * axis): within a byte the MSB is the LEFTMOST pixel of the 8-pixel group.
 * Bit set = black ink ("pixel on"); the engine inverts when it builds the
 * controller RAM stream, where 0x24-RAM 1 = white, 0 = black.
 *
 * 250 px / 8 = 31.25, so each row is 32 bytes with 6 PAD BITS which live
 * in the LOW 6 bits (LSBs) of the LAST byte (byte 31) of each row, i.e.
 * logical x = 250..255. They are never set (they stay "no ink").
 */
void test_fb_geometry_row_major_msb_first(void) {
    TEST_ASSERT_EQUAL_INT(3904, SSD1680_FB_SIZE);
    TEST_ASSERT_EQUAL_INT(32, SSD1680_FB_STRIDE);
    TEST_ASSERT_EQUAL_INT(122 * 32, SSD1680_FB_SIZE);

    const uint8_t* fb = ssd1680_engine_fb();

    /* (0,0): row 0, byte 0, MSB */
    ssd1680_engine_pixel(0, 0, true);
    TEST_ASSERT_EQUAL_HEX8(0x80, fb[0]);

    /* (7,0): same byte, LSB of the group */
    ssd1680_engine_pixel(7, 0, true);
    TEST_ASSERT_EQUAL_HEX8(0x81, fb[0]);

    /* (8,0): next byte, MSB */
    ssd1680_engine_pixel(8, 0, true);
    TEST_ASSERT_EQUAL_HEX8(0x80, fb[1]);

    /* (0,1): next row starts 32 bytes in (row-major, 32 B stride) */
    ssd1680_engine_pixel(0, 1, true);
    TEST_ASSERT_EQUAL_HEX8(0x80, fb[32]);

    /* (249,121): last pixel = last row, byte 31, bit 6 (x%8 == 1) */
    ssd1680_engine_pixel(249, 121, true);
    TEST_ASSERT_EQUAL_HEX8(0x40, fb[121 * 32 + 31]);

    /* pixel off clears the bit */
    ssd1680_engine_pixel(0, 0, false);
    TEST_ASSERT_EQUAL_HEX8(0x01, fb[0]);
}

void test_fb_pad_bits_stay_clear(void) {
    /* Fill every real pixel; the 6 pad bits per row (LSBs of byte 31)
     * must remain clear. */
    for (int y = 0; y < SSD1680_HEIGHT; y++)
        for (int x = 0; x < SSD1680_WIDTH; x++)
            ssd1680_engine_pixel(x, y, true);

    const uint8_t* fb = ssd1680_engine_fb();
    for (int y = 0; y < SSD1680_HEIGHT; y++) {
        for (int b = 0; b < 31; b++)
            TEST_ASSERT_EQUAL_HEX8(0xFF, fb[y * 32 + b]);
        /* byte 31 carries x=248 (bit7) and x=249 (bit6) only */
        TEST_ASSERT_EQUAL_HEX8(0xC0, fb[y * 32 + 31]);
    }
}

void test_pixel_out_of_bounds_ignored(void) {
    const uint8_t* fb = ssd1680_engine_fb();
    uint8_t before[SSD1680_FB_SIZE];
    memcpy(before, fb, SSD1680_FB_SIZE);

    ssd1680_engine_pixel(-1, 0, true);
    ssd1680_engine_pixel(0, -1, true);
    ssd1680_engine_pixel(250, 0, true);
    ssd1680_engine_pixel(0, 122, true);

    TEST_ASSERT_EQUAL_MEMORY(before, fb, SSD1680_FB_SIZE);
}

/* ── init / full refresh command stream ──────────────────────────────── */

void test_first_flush_is_full_with_datasheet_init_sequence(void) {
    const ssd1680_op_t* ops;
    size_t n;
    uint32_t busy;

    /* engine_init forces the first refresh FULL even with a clean fb:
     * the physical panel may hold a stale image. */
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_FULL, flush(&ops, &n, &busy));
    TEST_ASSERT_NOT_NULL(ops);
    TEST_ASSERT_TRUE(n >= 14);

    /* SW reset opens the stream (p.31 step 2: HW reset is the io layer's
     * job, then "SW Reset by Command 0x12"). */
    TEST_ASSERT_EQUAL_HEX8(0x12, ops[0].cmd);
    TEST_ASSERT_EQUAL_INT(0, (int)ops[0].len);

    /* Driver output control (p.31 step 3 "Set gate driver output by
     * Command 0x01"): MUX = 249 -> 250 gate lines (p.6: 250(V)),
     * GD = SM = TB = 0. */
    const ssd1680_op_t* op = find_op(ops, n, 0x01);
    TEST_ASSERT_NOT_NULL(op);
    TEST_ASSERT_EQUAL_INT(3, (int)op->len);
    TEST_ASSERT_EQUAL_HEX8(0xF9, op->data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, op->data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00, op->data[2]);

    /* Data entry mode (p.31 step 3 "Set display RAM size by Command 0x11,
     * 0x44, 0x45"): 0x03 = X increment, Y increment, advance along X. */
    op = find_op(ops, n, 0x11);
    TEST_ASSERT_NOT_NULL(op);
    TEST_ASSERT_EQUAL_INT(1, (int)op->len);
    TEST_ASSERT_EQUAL_HEX8(0x03, op->data[0]);

    /* RAM X window: the SSD1680 addresses X in BYTES (8 source pixels per
     * address unit). The panel has 122 sources (p.6: 122(H)), so the X
     * window is bytes 0x00..0x0F: 16 bytes = 128 source slots, of which
     * the LAST 6 (sources 122..127, the low bits of byte 15) are pad. */
    op = find_op(ops, n, 0x44);
    TEST_ASSERT_NOT_NULL(op);
    TEST_ASSERT_EQUAL_INT(2, (int)op->len);
    TEST_ASSERT_EQUAL_HEX8(0x00, op->data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x0F, op->data[1]);

    /* RAM Y window: gates 0..249 (250 gate lines, p.6), 16-bit LE pairs. */
    op = find_op(ops, n, 0x45);
    TEST_ASSERT_NOT_NULL(op);
    TEST_ASSERT_EQUAL_INT(4, (int)op->len);
    TEST_ASSERT_EQUAL_HEX8(0x00, op->data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, op->data[1]);
    TEST_ASSERT_EQUAL_HEX8(0xF9, op->data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00, op->data[3]);

    /* Border waveform (p.31 step 3 "Set panel border by Command 0x3C"):
     * 0x05 for full refresh (follow LUT, GS transition). */
    op = find_op(ops, n, 0x3C);
    TEST_ASSERT_NOT_NULL(op);
    TEST_ASSERT_EQUAL_INT(1, (int)op->len);
    TEST_ASSERT_EQUAL_HEX8(0x05, op->data[0]);

    /* Display update control 1: normal RAM content, source output mode
     * S8..S167 (the 122-source panel sits inside that window; matches the
     * panel vendor reference code). */
    op = find_op(ops, n, 0x21);
    TEST_ASSERT_NOT_NULL(op);
    TEST_ASSERT_EQUAL_INT(2, (int)op->len);
    TEST_ASSERT_EQUAL_HEX8(0x00, op->data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x80, op->data[1]);

    /* Temperature sensor: internal (p.31 step 4 "Sense temperature by
     * int/ext TS by Command 0x18"; 0x80 = internal sensor). */
    op = find_op(ops, n, 0x18);
    TEST_ASSERT_NOT_NULL(op);
    TEST_ASSERT_EQUAL_INT(1, (int)op->len);
    TEST_ASSERT_EQUAL_HEX8(0x80, op->data[0]);

    /* RAM counters reset to the window origin before the RAM write
     * (p.31 step 5 "Write image data in RAM by Command 0x4E, 0x4F,
     * 0x24, 0x26"). */
    op = find_op(ops, n, 0x4E);
    TEST_ASSERT_NOT_NULL(op);
    TEST_ASSERT_EQUAL_INT(1, (int)op->len);
    TEST_ASSERT_EQUAL_HEX8(0x00, op->data[0]);
    op = find_op(ops, n, 0x4F);
    TEST_ASSERT_NOT_NULL(op);
    TEST_ASSERT_EQUAL_INT(2, (int)op->len);
    TEST_ASSERT_EQUAL_HEX8(0x00, op->data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00, op->data[1]);

    /* Counters and windows precede the RAM write; RAM write precedes the
     * update trigger. */
    TEST_ASSERT_TRUE(op_index(ops, n, 0x44) < op_index(ops, n, 0x4E));
    TEST_ASSERT_TRUE(op_index(ops, n, 0x4E) < op_index(ops, n, 0x24));
    TEST_ASSERT_TRUE(op_index(ops, n, 0x24) < op_index(ops, n, 0x22));

    /* Full refresh drives Display Mode 1 with clock/analog enable, load
     * temperature, then power-down: 0x22 = 0xF7 (p.31 steps 4+5: LUT from
     * OTP and drive via 0x22, 0x20), then master activation 0x20. */
    op = find_op(ops, n, 0x22);
    TEST_ASSERT_NOT_NULL(op);
    TEST_ASSERT_EQUAL_INT(1, (int)op->len);
    TEST_ASSERT_EQUAL_HEX8(0xF7, op->data[0]);
    TEST_ASSERT_EQUAL_INT(op_index(ops, n, 0x22) + 1, op_index(ops, n, 0x20));
    TEST_ASSERT_EQUAL_INT(0, (int)find_op(ops, n, 0x20)->len);

    /* busy_ms seeded from p.10: full update 3 s typ. */
    TEST_ASSERT_EQUAL_UINT32(3000, busy);
    TEST_ASSERT_EQUAL_UINT32(SSD1680_BUSY_MS_FULL, busy);
}

void test_full_flush_writes_both_ram_planes(void) {
    const ssd1680_op_t* ops;
    size_t n;
    uint32_t busy;
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_FULL, flush(&ops, &n, &busy));

    /* Native RAM stream: 250 gate rows x 16 X-bytes = 4000 bytes. Full
     * refresh writes the image to BOTH the B/W RAM (0x24) and the "RED"/
     * previous-image RAM (0x26) so later Display Mode 2 partials diff
     * against a known base (p.31 step 5 lists 0x24 and 0x26). */
    const ssd1680_op_t* bw = find_op(ops, n, 0x24);
    const ssd1680_op_t* red = find_op(ops, n, 0x26);
    TEST_ASSERT_NOT_NULL(bw);
    TEST_ASSERT_NOT_NULL(red);
    TEST_ASSERT_EQUAL_INT(4000, (int)bw->len);
    TEST_ASSERT_EQUAL_INT(4000, (int)red->len);
    TEST_ASSERT_EQUAL_MEMORY(bw->data, red->data, 4000);
}

/*
 * Logical -> native RAM mapping. The panel RAM is portrait (X = 122
 * sources in 16 bytes, Y = 250 gates); the firmware framebuffer is
 * landscape (250x122). The engine rotates 90 degrees when building the
 * RAM stream: logical (lx, ly) -> source = 121 - ly, gate = lx. Within a
 * RAM byte the MSB is the LOWEST source address of the 8-source group.
 * Polarity per SSD1680 B/W RAM: 1 = white, 0 = black; logical fb bit set
 * (= black ink) therefore CLEARS the RAM bit. The 6 source pad bits
 * (sources 122..127 = low 6 bits of each row's byte 15) are written
 * WHITE (1).
 *
 * NOTE: the pager PCB's mirrored FPC connector is electrical only; no
 * software mirroring is applied here (hardware doc rule). If bring-up
 * shows a flipped image, change the one mapping expression in
 * ssd1680_engine.c, not the transport.
 */
void test_ram_mapping_rotation_and_polarity(void) {
    const ssd1680_op_t* ops;
    size_t n;
    uint32_t busy;

    /* Blank (all white) fb -> RAM all 0xFF including pad bits. */
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_FULL, flush(&ops, &n, &busy));
    const ssd1680_op_t* bw = find_op(ops, n, 0x24);
    TEST_ASSERT_NOT_NULL(bw);
    for (int i = 0; i < 4000; i++)
        TEST_ASSERT_EQUAL_HEX8(0xFF, bw->data[i]);

    /* Logical (0,0) black -> gate 0, source 121 -> RAM row 0, byte 15
     * (sources 120..127), bit 0x40 (source 121) cleared; the low 6 bits
     * of byte 15 stay white (pad). */
    ssd1680_engine_pixel(0, 0, true);
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_PARTIAL, flush(&ops, &n, &busy));
    bw = find_op(ops, n, 0x24);
    TEST_ASSERT_NOT_NULL(bw);
    TEST_ASSERT_EQUAL_HEX8(0xBF, bw->data[15]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, bw->data[0]);

    /* Logical (249,121) black -> gate 249, source 0 -> last RAM row,
     * byte 0, MSB cleared. */
    ssd1680_engine_pixel(249, 121, true);
    ssd1680_engine_pixel(0, 0, false);
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_PARTIAL, flush(&ops, &n, &busy));
    bw = find_op(ops, n, 0x24);
    TEST_ASSERT_EQUAL_HEX8(0x7F, bw->data[249 * 16 + 0]);
    TEST_ASSERT_EQUAL_HEX8(0xFF, bw->data[15]);

    /* All black: every RAM byte 0x00 except the 6 pad bits per row.
     * Changing the whole framebuffer trips the change-fraction heuristic,
     * so this flush is promoted to FULL (the RAM layout asserted below is
     * identical either way). */
    for (int y = 0; y < SSD1680_HEIGHT; y++)
        for (int x = 0; x < SSD1680_WIDTH; x++)
            ssd1680_engine_pixel(x, y, true);
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_FULL, flush(&ops, &n, &busy));
    bw = find_op(ops, n, 0x24);
    for (int row = 0; row < 250; row++) {
        for (int b = 0; b < 15; b++)
            TEST_ASSERT_EQUAL_HEX8(0x00, bw->data[row * 16 + b]);
        TEST_ASSERT_EQUAL_HEX8(0x3F, bw->data[row * 16 + 15]);
    }
}

/* ── partial refresh ─────────────────────────────────────────────────── */

void test_partial_flush_uses_display_mode_2(void) {
    const ssd1680_op_t* ops;
    size_t n;
    uint32_t busy;
    flush(&ops, &n, &busy); /* first: FULL */

    touch();
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_PARTIAL, flush(&ops, &n, &busy));

    /* No SW reset inside a partial (the io layer still HW-resets to wake
     * the controller from deep sleep; register state is re-sent below). */
    TEST_ASSERT_NULL(find_op(ops, n, 0x12));

    /* Partial keeps the border at VCOM (0x3C = 0x80) so the border does
     * not flicker on every partial update (panel vendor reference flow;
     * border command itself is p.31 step 3). */
    const ssd1680_op_t* op = find_op(ops, n, 0x3C);
    TEST_ASSERT_NOT_NULL(op);
    TEST_ASSERT_EQUAL_HEX8(0x80, op->data[0]);

    /* Display Mode 2 differential update: 0x22 = 0xFF, then 0x20
     * (p.31 step 5 "Drive display panel by Command 0x22, 0x20"). */
    op = find_op(ops, n, 0x22);
    TEST_ASSERT_NOT_NULL(op);
    TEST_ASSERT_EQUAL_HEX8(0xFF, op->data[0]);
    TEST_ASSERT_EQUAL_INT(op_index(ops, n, 0x22) + 1, op_index(ops, n, 0x20));

    /* Partial writes only the B/W RAM: the controller ping-pongs 0x24 into
     * the previous-image RAM after a Mode 2 update, so rewriting 0x26
     * every partial is not needed. */
    TEST_ASSERT_NOT_NULL(find_op(ops, n, 0x24));
    TEST_ASSERT_NULL(find_op(ops, n, 0x26));

    /* busy_ms seeded from p.10: partial update 0.42 s typ, rounded up. */
    TEST_ASSERT_EQUAL_UINT32(500, busy);
    TEST_ASSERT_EQUAL_UINT32(SSD1680_BUSY_MS_PARTIAL, busy);
}

/* ── refresh policy ──────────────────────────────────────────────────── */

void test_refresh_policy_every_10th_flush_full(void) {
    const ssd1680_op_t* ops;
    size_t n;
    uint32_t busy;

    /* p.10 recommends a full refresh after 5 consecutive partials to
     * limit ghosting; the policy constant is 10 per the emulator design
     * (single table in ssd1680_engine.h so bring-up can retune it). */
    TEST_ASSERT_EQUAL_INT(10, SSD1680_FULL_EVERY_N_FLUSHES);

    /* Refresh #1: FULL (forced by init). */
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_FULL, flush(&ops, &n, &busy));

    /* Refreshes #2..#10: PARTIAL. */
    for (int i = 2; i <= 10; i++) {
        touch();
        TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_PARTIAL, flush(&ops, &n, &busy));
    }

    /* Refresh #11 (every 10th after the first): FULL again. */
    touch();
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_FULL, flush(&ops, &n, &busy));

    /* And the cycle repeats. */
    for (int i = 2; i <= 10; i++) {
        touch();
        TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_PARTIAL, flush(&ops, &n, &busy));
    }
    touch();
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_FULL, flush(&ops, &n, &busy));
}

void test_clean_flush_is_none_and_does_not_advance_policy(void) {
    const ssd1680_op_t* ops;
    size_t n;
    uint32_t busy;
    flush(&ops, &n, &busy); /* FULL */

    /* Nothing changed since the last flush: no panel traffic at all. */
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_NONE, flush(&ops, &n, &busy));
    TEST_ASSERT_EQUAL_INT(0, (int)n);
    TEST_ASSERT_EQUAL_UINT32(0, busy);

    /* A pixel written to its existing value is not a change. */
    ssd1680_engine_pixel(5, 5, false);
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_NONE, flush(&ops, &n, &busy));

    /* NONE flushes do not count toward the every-10th-full cadence:
     * 9 real partials still fit before the next full. */
    for (int i = 2; i <= 10; i++) {
        touch();
        TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_PARTIAL, flush(&ops, &n, &busy));
        TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_NONE, flush(&ops, &n, &busy));
    }
    touch();
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_FULL, flush(&ops, &n, &busy));
}

/* ── power management ────────────────────────────────────────────────── */

void test_deep_sleep_terminates_every_stream(void) {
    const ssd1680_op_t* ops;
    size_t n;
    uint32_t busy;

    /* p.31 step 6 "Deep sleep by Command 0x10". Mode 1 (0x01) retains
     * RAM, which the Mode 2 partial diff depends on. Always the last op,
     * after master activation (the io layer waits BUSY low between 0x20
     * and 0x10, then leaves the panel asleep at ~uA until the next
     * flush's HW reset). */
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_FULL, flush(&ops, &n, &busy));
    TEST_ASSERT_EQUAL_HEX8(0x10, ops[n - 1].cmd);
    TEST_ASSERT_EQUAL_INT(1, (int)ops[n - 1].len);
    TEST_ASSERT_EQUAL_HEX8(0x01, ops[n - 1].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x20, ops[n - 2].cmd);

    touch();
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_PARTIAL, flush(&ops, &n, &busy));
    TEST_ASSERT_EQUAL_HEX8(0x10, ops[n - 1].cmd);
    TEST_ASSERT_EQUAL_HEX8(0x01, ops[n - 1].data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x20, ops[n - 2].cmd);
}

/* ââ change-fraction promotion ââ */

/* A large change since the last shown image (message scroll, screen or
 * menu switch) must promote the flush to FULL immediately, not wait for
 * the every-N cadence. */
void test_big_change_promotes_to_full(void) {
    const ssd1680_op_t* ops;
    size_t n;
    uint32_t busy;
    flush(&ops, &n, &busy); /* first: FULL, shown = blank */

    /* Small update: one pixel, well under the threshold -> PARTIAL. */
    ssd1680_engine_pixel(3, 3, true);
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_PARTIAL, flush(&ops, &n, &busy));

    /* Screen change: paint the top half black (~50% of bytes differ from
     * the shown image) -> promoted to FULL with the full busy window. */
    for (int y = 0; y < SSD1680_HEIGHT / 2; y++)
        for (int x = 0; x < SSD1680_WIDTH; x++)
            ssd1680_engine_pixel(x, y, true);
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_FULL, flush(&ops, &n, &busy));
    TEST_ASSERT_EQUAL_UINT32(SSD1680_BUSY_MS_FULL, busy);

    /* The promotion consumed the change: the next small update is PARTIAL
     * again (the shown shadow was updated at the full refresh). */
    ssd1680_engine_pixel(0, SSD1680_HEIGHT - 1, true);
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_PARTIAL, flush(&ops, &n, &busy));
}

/* The threshold boundary: just under SSD1680_FULL_CHANGE_PCT stays
 * partial; crossing it promotes. One fb byte covers 8 sources on one
 * gate, so painting whole gate rows moves the fraction in 16-byte steps
 * (16 of 4000 bytes per row). */
void test_change_fraction_threshold_boundary(void) {
    const ssd1680_op_t* ops;
    size_t n;
    uint32_t busy;
    flush(&ops, &n, &busy); /* first: FULL */

    /* Paint rows until just UNDER the threshold: each logical row y dirties
     * 32 bytes of the 3904-byte fb (0.82%); 23 rows = ~18.9% < 20%. */
    for (int y = 0; y < 23; y++)
        for (int x = 0; x < SSD1680_WIDTH; x++)
            ssd1680_engine_pixel(x, y, true);
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_PARTIAL, flush(&ops, &n, &busy));

    /* Two more rows (25 total = ~20.5%) crosses the 20% threshold -> FULL. */
    for (int y = 23; y < 25; y++)
        for (int x = 0; x < SSD1680_WIDTH; x++)
            ssd1680_engine_pixel(x, y, true);
    TEST_ASSERT_EQUAL_INT(SSD1680_REFRESH_FULL, flush(&ops, &n, &busy));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_fb_geometry_row_major_msb_first);
    RUN_TEST(test_fb_pad_bits_stay_clear);
    RUN_TEST(test_pixel_out_of_bounds_ignored);
    RUN_TEST(test_first_flush_is_full_with_datasheet_init_sequence);
    RUN_TEST(test_full_flush_writes_both_ram_planes);
    RUN_TEST(test_ram_mapping_rotation_and_polarity);
    RUN_TEST(test_partial_flush_uses_display_mode_2);
    RUN_TEST(test_big_change_promotes_to_full);
    RUN_TEST(test_change_fraction_threshold_boundary);
    RUN_TEST(test_refresh_policy_every_10th_flush_full);
    RUN_TEST(test_clean_flush_is_none_and_does_not_advance_policy);
    RUN_TEST(test_deep_sleep_terminates_every_stream);
    return UNITY_END();
}
