/*
 * Bramble SSD1680 e-paper SSI slave (QEMU esp32s3).
 *
 * Register-accurate model of the Solomon SSD1680 controller driving the pager's
 * GDEY0213B74 2.13" e-paper (components/display/ssd1680_io.c on top of the
 * shared ssd1680_engine.c). It decodes the command/data stream the firmware
 * clocks out, rebuilds the controller's image RAM, and on Master Activation
 * (0x20) unpacks that RAM into the SAME 250x122 1bpp logical framebuffer the
 * linux node ships (display_virt.c) and emits it to the gosim ether as an
 * emu-link `fb` message. The browser device view then renders the QEMU pager's
 * screen pixel-identical to a linux node.
 *
 * Command vs data is the D/C# line (GPIO5): ssd1680_io.c drives it low before a
 * command byte and high before data bytes, one spi transaction each, so we read
 * bramble_gpio_out_level(5) per clocked byte. Because a command and its data
 * arrive in SEPARATE CS transactions, the decode state (current command + its
 * data index) PERSISTS across CS deassert/reassert; unlike the SX1262 there is
 * no per-CS cursor reset, so this slave defines no set_cs.
 *
 * RAM addressing mirrors ssd1680_engine.c's data-entry-mode 0x03 sweep exactly,
 * so the captured RAM is byte-identical to the engine's s_ram: X in bytes
 * (0x44 window, 0x4E counter), gate rows (0x45 window, 0x4F counter), the write
 * cursor advancing X-within-row then row. The engine always writes the full
 * window from the origin in one linear sweep, so this reconstructs s_ram
 * bit-for-bit.
 *
 * The unpack (bramble_ssd1680_render_fb) is the exact inverse of the engine's
 * build_ram_stream rotation + polarity mapping: RAM bit 1 = white, so a cleared
 * bit is black ink; native source sx maps to logical ly = 121 - sx, native gate
 * gy maps to logical lx = gy. The result is byte-identical to
 * ssd1680_engine_fb(), so the `fb` payload matches the linux node's for
 * identical UI content. Only the BW plane (0x24) is rendered; the RED plane
 * (0x26, the Mode-2 diff base) is captured for completeness but does not affect
 * the visible mono image. seq/kind/busy_ms follow display_virt.c: seq starts at
 * 1, kind from the 0x22 Display-Update-Control-2 byte (0xF7 = full, 0xFF =
 * partial), busy_ms 3000 full / 500 partial (ssd1680_engine.h constants).
 */

#include "qemu/osdep.h"
#include "hw/qdev-core.h"
#include "qom/object.h"
#include "hw/ssi/ssi.h"
#include "hw/xtensa/bramble_ssd1680.h"
#include "hw/xtensa/bramble_gpio.h"
#include "hw/xtensa/bramble_emulink.h"

/* Geometry + RAM layout mirror ssd1680_engine.h (not includable from the QEMU
 * tree); keep in sync with that header. */
#define SSD1680_FB_W        250
#define SSD1680_FB_H        122
#define SSD1680_FB_STRIDE   32
#define SSD1680_FB_SIZE     (SSD1680_FB_STRIDE * SSD1680_FB_H)  /* 3904 */
#define SSD1680_RAM_STRIDE  16
#define SSD1680_RAM_SIZE    (SSD1680_RAM_STRIDE * SSD1680_FB_W) /* 4000 */

/* busy_ms seeds (ssd1680_engine.h SSD1680_BUSY_MS_*). */
#define SSD1680_BUSY_MS_FULL     3000u
#define SSD1680_BUSY_MS_PARTIAL  500u

/* D/C# line = GPIO5 (main/boards/bramble_pager.h epd_display.dc). */
#define SSD1680_DC_GPIO     5

/* SSD1680 commands the firmware sends (ssd1680_engine.h SSD1680_CMD_*). */
#define SSD1680_CMD_DRIVER_OUTPUT   0x01
#define SSD1680_CMD_DEEP_SLEEP      0x10
#define SSD1680_CMD_DATA_ENTRY      0x11
#define SSD1680_CMD_SW_RESET        0x12
#define SSD1680_CMD_TEMP_SENSOR     0x18
#define SSD1680_CMD_MASTER_ACTIVATE 0x20
#define SSD1680_CMD_UPDATE_CTRL1    0x21
#define SSD1680_CMD_UPDATE_CTRL2    0x22
#define SSD1680_CMD_WRITE_RAM_BW    0x24
#define SSD1680_CMD_WRITE_RAM_RED   0x26
#define SSD1680_CMD_BORDER          0x3C
#define SSD1680_CMD_RAM_X_WINDOW    0x44
#define SSD1680_CMD_RAM_Y_WINDOW    0x45
#define SSD1680_CMD_RAM_X_COUNTER   0x4E
#define SSD1680_CMD_RAM_Y_COUNTER   0x4F

/* Display Update Control 2 (0x22) payloads (ssd1680_engine.c d_duc2_*). */
#define SSD1680_DUC2_FULL     0xF7
#define SSD1680_DUC2_PARTIAL  0xFF

OBJECT_DECLARE_SIMPLE_TYPE(BrambleSsd1680State, BRAMBLE_SSD1680)

struct BrambleSsd1680State {
    SSIPeripheral parent_obj;

    /* Controller image RAM, rebuilt by WRITE_RAM (0x24 BW / 0x26 RED). Only BW
     * is rendered; RED is the Mode-2 diff base, captured for completeness. */
    uint8_t bw_ram[SSD1680_RAM_SIZE];
    uint8_t red_ram[SSD1680_RAM_SIZE];

    /* RAM window (bytes for X, gate rows for Y) and address counter, set by
     * 0x44/0x45 and 0x4E/0x4F; the write cursor is seeded from the counter. */
    uint8_t x_start, x_end;    /* X window, byte units (0x44) */
    uint16_t y_start, y_end;   /* Y window, gate rows (0x45) */
    uint8_t cur_xb;            /* X address counter, byte units (0x4E) */
    uint16_t cur_gate;         /* Y address counter, gate row (0x4F) */
    uint8_t wr_xb;             /* live write cursor X, seeded from cur_xb */
    uint16_t wr_gate;          /* live write cursor gate, seeded from cur_gate */

    /* Command decode state, PERSISTS across CS cycles (a command and its data
     * arrive in separate transactions). */
    uint8_t cmd;               /* current command byte (D/C# low) */
    uint32_t data_idx;         /* data-byte index within the current command */
    uint8_t duc2;              /* latched Display Update Control 2 (0x22) */

    uint32_t seq;              /* emu-link `fb` seq, first frame = 1 */

    /* Render scratch: the unpacked logical framebuffer shipped in `fb`. */
    uint8_t fb[SSD1680_FB_SIZE];
};

/* Store one WRITE_RAM data byte at the live write cursor and advance it in
 * data-entry-mode 0x03 order (X byte within the window, then gate row), the
 * same sweep ssd1680_engine.c emits. Out-of-range writes are dropped. */
static void bramble_ssd1680_ram_write(BrambleSsd1680State *s, uint8_t *ram,
                                      uint8_t byte)
{
    uint32_t off = (uint32_t)s->wr_gate * SSD1680_RAM_STRIDE + s->wr_xb;
    if (off < SSD1680_RAM_SIZE) {
        ram[off] = byte;
    }
    if (s->wr_xb >= s->x_end) {
        s->wr_xb = s->x_start;
        s->wr_gate++;
    } else {
        s->wr_xb++;
    }
}

/* Unpack the BW image RAM into the 250x122 1bpp logical framebuffer, the exact
 * inverse of ssd1680_engine.c build_ram_stream: native gate gy = logical lx,
 * native source sx = logical ly (121 - sx); RAM polarity 1 = white, so a CLEARED
 * RAM bit is black ink. Produces bytes identical to ssd1680_engine_fb(). */
static void bramble_ssd1680_render_fb(BrambleSsd1680State *s)
{
    memset(s->fb, 0, sizeof(s->fb));
    for (int gy = 0; gy < SSD1680_FB_W; gy++) {   /* gate row = logical x */
        int lx = gy;
        for (int xb = 0; xb < SSD1680_RAM_STRIDE; xb++) {
            uint8_t ram = s->bw_ram[gy * SSD1680_RAM_STRIDE + xb];
            for (int bit = 0; bit < 8; bit++) {
                int sx = xb * 8 + bit;            /* source line */
                if (sx >= SSD1680_FB_H) {
                    break;                        /* pad sources 122..127 */
                }
                int ly = SSD1680_FB_H - 1 - sx;   /* logical y */
                if ((ram & (0x80u >> bit)) == 0) { /* RAM 0 = black ink */
                    s->fb[ly * SSD1680_FB_STRIDE + lx / 8] |=
                        (uint8_t)(0x80u >> (lx & 7));
                }
            }
        }
    }
}

static uint32_t bramble_ssd1680_transfer(SSIPeripheral *dev, uint32_t val)
{
    BrambleSsd1680State *s = BRAMBLE_SSD1680(dev);
    uint8_t byte = val & 0xff;

    /* D/C# low = command, high = data (ssd1680_io.c drives GPIO5 before each
     * transaction). The display never reads back, so MISO is always 0. */
    if (bramble_gpio_out_level(SSD1680_DC_GPIO) == 0) {
        s->cmd = byte;
        s->data_idx = 0;
        switch (byte) {
        case SSD1680_CMD_WRITE_RAM_BW:
        case SSD1680_CMD_WRITE_RAM_RED:
            /* Seed the write cursor from the address counter (0x4E/0x4F set
             * just before): the engine writes the full window from origin. */
            s->wr_xb = s->cur_xb;
            s->wr_gate = s->cur_gate;
            break;
        case SSD1680_CMD_MASTER_ACTIVATE:
            /* Drive the panel: unpack RAM -> logical fb and ship it. */
            bramble_ssd1680_render_fb(s);
            emulink_send_fb(s->fb, sizeof(s->fb), ++s->seq,
                            s->duc2 == SSD1680_DUC2_FULL ? "full" : "partial",
                            s->duc2 == SSD1680_DUC2_FULL
                                ? SSD1680_BUSY_MS_FULL
                                : SSD1680_BUSY_MS_PARTIAL);
            break;
        default:
            break;
        }
        return 0x00;
    }

    /* Data byte for the current command. */
    uint32_t idx = s->data_idx++;
    switch (s->cmd) {
    case SSD1680_CMD_RAM_X_WINDOW:    /* [x_start][x_end], byte units */
        if (idx == 0) {
            s->x_start = byte;
        } else if (idx == 1) {
            s->x_end = byte;
        }
        break;
    case SSD1680_CMD_RAM_Y_WINDOW:    /* [startL][startH][endL][endH], gates */
        if (idx == 0) {
            s->y_start = byte;
        } else if (idx == 1) {
            s->y_start |= (uint16_t)byte << 8;
        } else if (idx == 2) {
            s->y_end = byte;
        } else if (idx == 3) {
            s->y_end |= (uint16_t)byte << 8;
        }
        break;
    case SSD1680_CMD_RAM_X_COUNTER:   /* [x], byte units */
        if (idx == 0) {
            s->cur_xb = byte;
        }
        break;
    case SSD1680_CMD_RAM_Y_COUNTER:   /* [gateL][gateH] */
        if (idx == 0) {
            s->cur_gate = byte;
        } else if (idx == 1) {
            s->cur_gate |= (uint16_t)byte << 8;
        }
        break;
    case SSD1680_CMD_WRITE_RAM_BW:
        bramble_ssd1680_ram_write(s, s->bw_ram, byte);
        break;
    case SSD1680_CMD_WRITE_RAM_RED:
        bramble_ssd1680_ram_write(s, s->red_ram, byte);
        break;
    case SSD1680_CMD_UPDATE_CTRL2:    /* [duc2]: latch the full/partial kind */
        if (idx == 0) {
            s->duc2 = byte;
        }
        break;
    default:
        /* 0x01, 0x11, 0x18, 0x21, 0x3C, 0x10, ...: accepted, no state. */
        break;
    }
    return 0x00;
}

static void bramble_ssd1680_realize(SSIPeripheral *dev, Error **errp)
{
    BrambleSsd1680State *s = BRAMBLE_SSD1680(dev);
    (void)errp;
    /* Panel powers up with white RAM (1 = white); default the window to the
     * whole panel so a render before the first 0x44/0x45 stays in-bounds. */
    memset(s->bw_ram, 0xFF, sizeof(s->bw_ram));
    memset(s->red_ram, 0xFF, sizeof(s->red_ram));
    s->x_start = 0x00;
    s->x_end = SSD1680_RAM_STRIDE - 1;
    s->y_start = 0;
    s->y_end = SSD1680_FB_W - 1;
    s->cur_xb = 0;
    s->cur_gate = 0;
    s->wr_xb = 0;
    s->wr_gate = 0;
    s->data_idx = 0;
    s->duc2 = SSD1680_DUC2_FULL;
    s->seq = 0;
}

static void bramble_ssd1680_class_init(ObjectClass *klass, void *data)
{
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    k->realize = bramble_ssd1680_realize;
    k->transfer = bramble_ssd1680_transfer;
    /* CS-gated: bramble_gpspi2_route drives this slave's SSI_GPIO_CS from the
     * hardware-CS0 decision so it only answers display transactions. No set_cs:
     * unlike the SX1262 a command and its data span separate CS cycles and the
     * decode state must persist across them. */
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo bramble_ssd1680_info = {
    .name = TYPE_BRAMBLE_SSD1680,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(BrambleSsd1680State),
    .class_init = bramble_ssd1680_class_init,
};

static void bramble_ssd1680_register_types(void)
{
    type_register_static(&bramble_ssd1680_info);
}

type_init(bramble_ssd1680_register_types)
