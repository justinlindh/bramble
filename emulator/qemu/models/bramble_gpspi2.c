/*
 * Bramble GPSPI2 (SPI2_HOST) controller model (QEMU esp32s3, Phase 2 P2.3).
 *
 * The espressif/qemu esp32s3 machine models only the flash MEM controller
 * (hw/ssi/esp32s3_spi.c: SPI_MEM_* / FLASH_* registers at DR_REG_SPI1_BASE),
 * which is why flash reads / NVS / boot work. The pager's radio (sx1262.c) and
 * e-paper (ssd1680_io.c) hang off GPSPI2 (SPI2_HOST), a SEPARATE general-purpose
 * SPI peripheral at DR_REG_SPI2_BASE (0x60024000) that the stock machine does
 * not model or map. Reads/writes to that window hit the catch-all IO region,
 * the SPI "user transaction done" bit never clears, and the IDF spi_master
 * polling driver spins forever (spi_device_polling_transmit ->
 * spi_hal_usr_is_done). Boot wedges in show_splash at the first e-paper command.
 *
 * This model overlays the GPSPI2 register window (like bramble_gpio overlays the
 * GPIO window) and implements just enough of the spi_master user-transaction
 * surface that the pager's transfers COMPLETE, so boot proceeds into the main
 * loop. It exposes an SSI bus with a stub slave attached (drains MOSI, returns a
 * benign 0x00 MISO byte); register-accurate SX1262 (P2.4) and SSD1680 (P2.5)
 * slaves replace the stub later.
 *
 * Done detection. The pager uses spi_device_polling_transmit for EVERY transfer
 * (both e-paper and, later, radio), which polls SPI_CMD_REG.SPI_USR to self-
 * clear. We perform the whole transfer synchronously when SPI_USR is written and
 * leave the bit clear, so the poll sees "done" immediately. Likewise
 * SPI_CMD.SPI_UPDATE (the CONF-sync bit the driver spins on before every
 * transfer) always reads back 0. The transfer-done interrupt is wired to
 * ETS_SPI2_INTR_SOURCE for completeness, but the pager never enables or takes
 * it, so it stays quiescent.
 *
 * Two data paths, routed by the per-transfer DMA-enable bits in SPI_DMA_CONF_REG
 * exactly as the real controller routes them:
 *   - CPU / register path (SPI_DMA_TX_ENA clear): small transfers that IDF marks
 *     SPI_TRANS_USE_TXDATA put their bytes in SPI_W0..W15 (the 64-byte CPU data
 *     buffer). Every e-paper COMMAND and short (<=4 byte) register payload takes
 *     this path, including the first wedging op (epd_write_cmd(0x12), 8 bits).
 *     We shift the W-buffer bytes out through ssi_transfer and capture MISO back
 *     into the W buffer.
 *   - GDMA path (SPI_DMA_TX_ENA set): the e-paper framebuffer chunks (up to 240
 *     bytes, tx_buffer pointer, no USE_TXDATA) stream through GDMA. We pull the
 *     MOSI bytes from the SPI2 GDMA OUT channel (esp_gdma_read_channel) and, if
 *     MISO is enabled, push captured bytes back via the IN channel. See the
 *     CS-routing / channel-disambiguation notes below.
 *
 * CS routing (implemented in P2.4a, refined by P2.5). The pager mixes CS
 * styles, and two register-accurate slaves share the bus (SX1262 radio +
 * SSD1680 display), so every transfer must go to exactly one:
 *   - Radio (sx1262.c) uses MANUAL software CS: spics_io_num = -1 and the driver
 *     toggles gpio_set_level(GPIO8) by hand; the P2.2 bramble_gpio overlay
 *     observes GPIO8. So the radio slave's select is derived from the GPIO8
 *     level read back through bramble_gpio_out_level(), NOT from this
 *     peripheral's CS lines. When GPIO8 is low the transfer routes to the
 *     register-accurate SX1262 slave (TYPE_BRAMBLE_SX1262, defined below).
 *   - Display (ssd1680_io.c) uses HARDWARE CS: spics_io_num = GPIO4. The
 *     display is added first (display_init runs before radio_init), so it is
 *     SPI device 0 and owns CS0; the IDF spi_master enables it per transaction
 *     via SPI_MISC_REG.CS0_DIS. P2.5 selects the display POSITIVELY off that
 *     bit (CS0_DIS clear) rather than the P2.4 stub's "GPIO8 not low"
 *     simplification: disp_sel = (GPIO8 high) AND (CS0 enabled). The SSD1680
 *     slave (register-accurate, TYPE_BRAMBLE_SSD1680) is defined below.
 * Both slaves are SSI_CS_LOW; bramble_gpspi2_route drives their SSI_GPIO_CS
 * inputs from that decision so exactly one answers each ssi_transfer. The radio
 * routing is unchanged from P2.4.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-core.h"
#include "qom/object.h"
#include "exec/address-spaces.h"
#include "qemu/cutils.h"
#include "hw/ssi/ssi.h"
#include "hw/dma/esp_gdma.h"
#include "hw/xtensa/bramble_gpspi2.h"
#include "hw/xtensa/bramble_gpio.h"
#include "hw/xtensa/bramble_scaffold.h"
#include "hw/misc/esp32s3_reg.h"
#include "hw/xtensa/esp32s3_intc.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qobject.h"
#include "qemu/base64.h"

/* GPSPI2 register offsets (soc/spi_reg.h, esp32s3: REG_SPI_BASE(2) window).
 * NB: this is the GENERAL-PURPOSE SPI register map, distinct from the flash
 * MEM controller's SPI_MEM_* map that hw/ssi/esp32s3_spi.c models. */
#define R_SPI_CMD          0x000
#define R_SPI_ADDR         0x004
#define R_SPI_CTRL         0x008
#define R_SPI_CLOCK        0x00C
#define R_SPI_USER         0x010
#define R_SPI_USER1        0x014
#define R_SPI_USER2        0x018
#define R_SPI_MS_DLEN      0x01C
#define R_SPI_MISC         0x020
#define R_SPI_DMA_CONF     0x030
#define R_SPI_DMA_INT_ENA  0x034
#define R_SPI_DMA_INT_CLR  0x038
#define R_SPI_DMA_INT_RAW  0x03C
#define R_SPI_DMA_INT_ST   0x040
#define R_SPI_DMA_INT_SET  0x044
#define R_SPI_W0           0x098
#define R_SPI_W15          0x0D4
#define R_SPI_SLAVE        0x0E0
#define R_SPI_SLAVE1       0x0E4
#define R_SPI_CLK_GATE     0x0E8

/* SPI_CMD_REG bits (this register map, NOT the flash one). */
#define SPI_CMD_USR        (1u << 24) /* R/W/SC: kick a user transaction */
#define SPI_CMD_UPDATE     (1u << 23) /* WT: sync CONF regs; self-clearing */

/* SPI_USER_REG phase-enable bits. */
#define SPI_USER_COMMAND   (1u << 31)
#define SPI_USER_ADDR      (1u << 30)
#define SPI_USER_DUMMY     (1u << 29)
#define SPI_USER_MISO      (1u << 28)
#define SPI_USER_MOSI      (1u << 27)

/* SPI_USER2_REG: command value [15:0] and (bitlen-1) [31:28]. */
#define SPI_USER2_CMD_VALUE_MASK   0x0000FFFFu
#define SPI_USER2_CMD_BITLEN_SHIFT 28
#define SPI_USER2_CMD_BITLEN_MASK  0xFu

/* SPI_MS_DLEN_REG: data length in bits, minus 1, in [17:0]. */
#define SPI_MS_DATA_BITLEN_MASK    0x0003FFFFu

/* SPI_MISC_REG chip-select disable (active-low CS0 line). */
#define SPI_MISC_CS0_DIS   (1u << 0)

/* SPI_DMA_CONF_REG data-path enables. */
#define SPI_DMA_TX_ENA     (1u << 28)
#define SPI_DMA_RX_ENA     (1u << 27)

/* SPI_DMA_INT_* transfer-done bit. */
#define SPI_TRANS_DONE_INT (1u << 12)

/* CPU data buffer: 16 words = 64 bytes (SPI_W0..W15). */
#define GPSPI2_BUF_WORDS   16
#define GPSPI2_BUF_BYTES   (GPSPI2_BUF_WORDS * 4)

/* Largest real SPI2 data transfer the pager issues: the 240-byte e-paper
 * framebuffer GDMA chunk (see the header note). A user transaction up to this
 * size shifts through stack buffers with no per-transaction allocation; a
 * larger one (never observed) falls back to the heap. */
#define GPSPI2_MAX_XFER    240

/* ---- emu-link bridge (P2.4b) --------------------------------------------- */
/*
 * The QEMU pager's single emu-link connection to the gosim ether, shared by the
 * device models (only the SX1262 uses it today). The QEMU node is an emu-link
 * client exactly like the linux node (components/emu_link/emu_link.c): JSON, one
 * object per line, over a socket. Here the socket is a QEMU chardev ("emulink")
 * the gosim supervisor wires with
 *   -chardev socket,id=emulink,path=<broker.sock>,server=off
 * so QEMU dials the broker's unix listener the way the linux node dials
 * EMU_BROKER. The broker is one-hello-per-node, so there is exactly one link.
 *
 * On socket open (CHR_EVENT_OPENED) it sends hello{node,version:1,fw,caps};
 * inbound lines are split on '\n', parsed with QEMU's qjson, and dispatched by
 * their "t" field to handlers device models register (emulink_on); the SX1262
 * model calls emulink_send_tx() to key the channel. node id comes from the
 * BRAMBLE_EMU_NODE env var the supervisor sets per instance (default
 * "qemu-pager"); the broker binds a node to a reserved SLOT by position, not by
 * this id, so it only affects UI / console tagging.
 *
 * This lives in bramble_gpspi2.c rather than its own translation unit because
 * hw/xtensa/meson.build is saturated: the P2.2-P2.3b patches already pack it
 * with three separated single-line add()s whose 3-line context windows leave no
 * gap for a fourth without breaking a neighbour's idempotent reverse-check
 * (bootstrap-qemu.sh). Folding the bridge into the already-compiled TU that owns
 * its only client (the SX1262 slave) sidesteps that entirely; the sole public
 * entry point, bramble_emulink_attach(), is declared in bramble_gpspi2.h.
 *
 * Threading: the chardev receive/event callbacks run on the QEMU main loop with
 * the BQL held, and the SX1262 SPI transfers that call emulink_send_tx run on
 * the vCPU thread under the BQL, so the BQL serializes the two and the shared
 * state needs no extra lock. qemu_chr_fe_write_all is documented thread-safe.
 */

#define EMULINK_PROTOCOL_VERSION 1
#define EMULINK_CHARDEV_ID       "emulink"
#define EMULINK_MAX_HANDLERS     8
#define EMULINK_MAX_TYPE_LEN     15
#define EMULINK_MAX_LINE         65536

typedef void (*emulink_handler_t)(QDict *msg, void *ctx);

typedef struct {
    bool used;
    char type[EMULINK_MAX_TYPE_LEN + 1];
    emulink_handler_t fn;
    void *ctx;
} EmulinkHandler;

static CharBackend s_emulink_chr;
static bool s_emulink_have_chr;
static bool s_emulink_open;
static GString *s_emulink_rxbuf;
static EmulinkHandler s_emulink_handlers[EMULINK_MAX_HANDLERS];

static int emulink_write(const char *s, size_t len)
{
    if (!s_emulink_have_chr || !s_emulink_open) {
        return -1;
    }
    return qemu_chr_fe_write_all(&s_emulink_chr, (const uint8_t *)s, (int)len);
}

/* Emit a `tx`: PHY bytes base64-encoded, plus the latched modulation params
 * (freq MHz, sf, bw Hz, cr, power dBm). No-op if the link is not connected, so
 * a standalone boot's radio simply never gets txdone and times out. */
static int emulink_send_tx(const uint8_t *payload, unsigned len, int freq_mhz,
                           int sf, int bw_hz, int cr, int power)
{
    if (!payload || len == 0) {
        return -1;
    }
    g_autofree char *b64 = g_base64_encode(payload, len);
    g_autofree char *line = g_strdup_printf(
        "{\"t\":\"tx\",\"payload\":\"%s\",\"freq\":%d,\"sf\":%d,\"bw\":%d,"
        "\"cr\":%d,\"power\":%d}\n",
        b64, freq_mhz, sf, bw_hz, cr, power);
    return emulink_write(line, strlen(line));
}

/* Emit an `fb`: the resolved 1bpp logical framebuffer base64-encoded, plus the
 * refresh kind ("full"/"partial") and busy duration, matching display_virt.c's
 * message shape exactly so the browser renders the QEMU pager's e-paper
 * identically to a linux node. No-op if the link is not connected. */
static int emulink_send_fb(const uint8_t *fb, size_t fb_len, uint32_t seq,
                           const char *kind, uint32_t busy_ms)
{
    if (!fb || fb_len == 0) {
        return -1;
    }
    g_autofree char *b64 = g_base64_encode(fb, fb_len);
    g_autofree char *line = g_strdup_printf(
        "{\"t\":\"fb\",\"seq\":%u,\"kind\":\"%s\",\"fb\":\"%s\","
        "\"busy_ms\":%u}\n",
        seq, kind, b64, busy_ms);
    return emulink_write(line, strlen(line));
}

/* ---- indicator bridge (LED / vibra / buzzer -> emu-link `ind`) ----------- */
/*
 * The QEMU pager runs the REAL indicators.c, which drives hardware the browser
 * cannot see: LED = GPIO48, vibra = GPIO16 (both gpio_set_level), buzzer =
 * GPIO15 via an LEDC tone. The linux node instead runs indicator_virt.c, which
 * emits one emu-link `ind` message carrying the FULL (led, buzzer_hz, vibra)
 * state on every change (components/indicators/indicator_virt.c). This bridge
 * reproduces that message for the QEMU node so the device view's sound /
 * vibration / LED cues fire identically:
 *   - LED + vibra come from the bramble_gpio OUT observer (the levels the
 *     firmware drives on GPIO48 / GPIO16);
 *   - buzzer comes from the minimal LEDC model below (channel-1 duty on/off).
 * State is debounced: an `ind` is sent only when led, vibra, or buzzer_hz
 * actually changes, and it always carries the full snapshot, matching
 * indicator_virt.c's send_state_locked exactly.
 *
 * buzzer_hz is reported as ALERT_BUZZER_HZ (the single tone the firmware ever
 * plays, components/indicators/include/alerts.h) when channel 1 is driven, else
 * 0. The exact tone is NOT recovered from the LEDC clock divider: the pager
 * lets the driver auto-select the low-speed source (default RC_FAST, which is
 * imprecise and unmodeled in QEMU), so on/off is the load-bearing, reliable
 * signal for "sound is functioning" and the tone is pinned to the known
 * firmware constant. See the LEDC model note below.
 *
 * Threading: note_gpio runs from the GPIO register-write path and note_buzzer
 * from the LEDC register-write path, both on a vCPU thread under the BQL, so
 * the shared snapshot needs no extra lock (same discipline as emulink_send_tx).
 */

#define BRAMBLE_IND_LED_GPIO   48
#define BRAMBLE_IND_VIBRA_GPIO 16
#define BRAMBLE_IND_BUZZER_HZ  3200u /* alerts.h ALERT_BUZZER_HZ; on/off cue */

static bool s_ind_led;
static uint32_t s_ind_buzzer_hz;
static bool s_ind_vibra;

/* Emit the current full (led, buzzer_hz, vibra) state as one `ind`, matching
 * indicator_virt.c's shape exactly. No-op if the link is not connected. */
static void emulink_send_ind(void)
{
    g_autofree char *line = g_strdup_printf(
        "{\"t\":\"ind\",\"led\":%s,\"buzzer_hz\":%u,\"vibra\":%s}\n",
        s_ind_led ? "true" : "false", s_ind_buzzer_hz,
        s_ind_vibra ? "true" : "false");
    (void)emulink_write(line, strlen(line));
}

/* GPIO OUT observer: forward LED (GPIO48) / vibra (GPIO16) transitions; ignore
 * every other output pin. Emits a full `ind` snapshot only on an actual
 * change (the overlay already suppresses no-op transitions). */
static void bramble_ind_note_gpio(int pin, bool level)
{
    bool changed = false;
    if (pin == BRAMBLE_IND_LED_GPIO) {
        if (s_ind_led != level) {
            s_ind_led = level;
            changed = true;
        }
    } else if (pin == BRAMBLE_IND_VIBRA_GPIO) {
        if (s_ind_vibra != level) {
            s_ind_vibra = level;
            changed = true;
        }
    }
    if (changed) {
        emulink_send_ind();
    }
}

/* LEDC model callback: buzzer tone (Hz) while channel 1 is driving, else 0.
 * Debounced against the last reported value. */
static void bramble_ind_note_buzzer(uint32_t hz)
{
    if (s_ind_buzzer_hz == hz) {
        return;
    }
    s_ind_buzzer_hz = hz;
    emulink_send_ind();
}

static int emulink_on(const char *type, emulink_handler_t fn, void *ctx)
{
    if (!type || !fn || strlen(type) > EMULINK_MAX_TYPE_LEN) {
        return -1;
    }
    int free_slot = -1;
    for (int i = 0; i < EMULINK_MAX_HANDLERS; i++) {
        if (s_emulink_handlers[i].used &&
            strcmp(s_emulink_handlers[i].type, type) == 0) {
            s_emulink_handlers[i].fn = fn;
            s_emulink_handlers[i].ctx = ctx;
            return 0;
        }
        if (!s_emulink_handlers[i].used && free_slot < 0) {
            free_slot = i;
        }
    }
    if (free_slot < 0) {
        return -1;
    }
    s_emulink_handlers[free_slot].used = true;
    pstrcpy(s_emulink_handlers[free_slot].type,
            sizeof(s_emulink_handlers[free_slot].type), type);
    s_emulink_handlers[free_slot].fn = fn;
    s_emulink_handlers[free_slot].ctx = ctx;
    return 0;
}

static void emulink_send_hello(void)
{
    const char *node = getenv("BRAMBLE_EMU_NODE");
    if (!node || !*node) {
        node = "qemu-pager";
    }
    g_autofree char *hello = g_strdup_printf(
        "{\"t\":\"hello\",\"node\":\"%s\",\"version\":%d,\"fw\":\"qemu\","
        "\"caps\":\"radio,display,buttons,gps,battery\"}\n",
        node, EMULINK_PROTOCOL_VERSION);
    (void)emulink_write(hello, strlen(hello));
    fprintf(stderr, "bramble-emulink: hello sent as node=%s\n", node);
}

static void emulink_dispatch_line(const char *line)
{
    if (!*line) {
        return;
    }
    QObject *obj = qobject_from_json(line, NULL);
    if (!obj) {
        return; /* malformed: ignore (forward compat) */
    }
    QDict *msg = qobject_to(QDict, obj);
    if (msg) {
        const char *t = qdict_get_try_str(msg, "t");
        if (t) {
            for (int i = 0; i < EMULINK_MAX_HANDLERS; i++) {
                if (s_emulink_handlers[i].used &&
                    strcmp(s_emulink_handlers[i].type, t) == 0) {
                    s_emulink_handlers[i].fn(msg, s_emulink_handlers[i].ctx);
                    break;
                }
            }
        }
    }
    qobject_unref(obj);
}

static int emulink_can_receive(void *opaque)
{
    (void)opaque;
    return EMULINK_MAX_LINE;
}

static void emulink_receive(void *opaque, const uint8_t *buf, int size)
{
    (void)opaque;
    for (int i = 0; i < size; i++) {
        char c = (char)buf[i];
        if (c == '\n') {
            emulink_dispatch_line(s_emulink_rxbuf->str);
            g_string_truncate(s_emulink_rxbuf, 0);
        } else {
            if (s_emulink_rxbuf->len >= EMULINK_MAX_LINE) {
                g_string_truncate(s_emulink_rxbuf, 0); /* oversized: resync */
            }
            g_string_append_c(s_emulink_rxbuf, c);
        }
    }
}

static void emulink_event(void *opaque, QEMUChrEvent event)
{
    (void)opaque;
    switch (event) {
    case CHR_EVENT_OPENED:
        s_emulink_open = true;
        g_string_truncate(s_emulink_rxbuf, 0);
        emulink_send_hello();
        break;
    case CHR_EVENT_CLOSED:
        s_emulink_open = false;
        break;
    default:
        break;
    }
}

/* Defined in the LEDC model section below; attached here so the whole
 * indicator bridge shares this one already-wired machine-init call site. */
static void bramble_ledc_attach(MemoryRegion *sys_mem);

void bramble_emulink_attach(void)
{
    /* Install the indicator bridge alongside the emu-link connection: the LEDC
     * buzzer overlay and the GPIO OUT observer both feed emu-link `ind`. Done
     * from this existing attach site (not a separate machine-init hook) so no
     * new wiring patch is needed; the overlays are harmless with no chardev (an
     * `ind` send is then a no-op). Registered before the chardev lookup so they
     * are installed even on a standalone boot. */
    bramble_ledc_attach(get_system_memory());
    bramble_gpio_set_out_observer(bramble_ind_note_gpio);

    Chardev *chr = qemu_chr_find(EMULINK_CHARDEV_ID);
    if (!chr) {
        fprintf(stderr, "bramble-emulink: no '%s' chardev; ether bridge idle\n",
                EMULINK_CHARDEV_ID);
        return;
    }
    if (!qemu_chr_fe_init(&s_emulink_chr, chr, &error_abort)) {
        fprintf(stderr, "bramble-emulink: chardev init failed\n");
        return;
    }
    s_emulink_have_chr = true;
    s_emulink_rxbuf = g_string_new(NULL);
    /* set_open=true replays a CHR_EVENT_OPENED if the socket connected during
     * option parse (server=off dials immediately), so hello is not missed. */
    qemu_chr_fe_set_handlers(&s_emulink_chr, emulink_can_receive,
                             emulink_receive, emulink_event, NULL, NULL, NULL,
                             true);
    fprintf(stderr, "bramble-emulink: bridge attached to '%s' chardev\n",
            EMULINK_CHARDEV_ID);
}

/* ---- minimal LEDC model (buzzer tone on/off) ----------------------------- */
/*
 * The pager's buzzer is GPIO15 driven by an LEDC PWM tone (indicators.c:
 * LEDC_LOW_SPEED_MODE, timer 1, channel 1). The stock esp32s3 QEMU does not
 * model LEDC at all: the peripheral window (DR_REG_LEDC_BASE) hits the catch-all
 * IO region, so writes vanish and reads return 0. The P2.2 GPIO overlay
 * therefore never sees the buzzer (it is not a gpio_set_level pin), and no
 * emu-link `ind` ever carried it.
 *
 * This overlay models just the channel-1 registers needed to tell whether the
 * tone is sounding: the output-enable bit (LSCH1_CONF0.SIG_OUT_EN) and the duty
 * (LSCH1_DUTY, 0 = silent). On any write to those, or to LSCH1_CONF1 (the
 * update-duty commit), it recomputes on = enabled && duty != 0 and reports the
 * tone to the indicator bridge (BRAMBLE_IND_BUZZER_HZ when on, 0 when off). The
 * pager's stop path is indicator_buzzer(0) -> ledc_set_duty(0) + update, so
 * duty == 0 is a reliable "silent" edge without modelling ledc_stop.
 *
 * It is a strict superset of the catch-all it overlays: only CONF0, DUTY and
 * the global LEDC_CONF register are latched and read back (with the write-only
 * self-clearing PARA_UP / OVF_CNT bits masked so a read never returns them set);
 * every other LEDC register reads 0 and drops writes exactly as before, so no
 * LEDC config or update poll that boot already relied on can newly spin. The
 * tone frequency is not derived from the timer divider (see the indicator-bridge
 * note): on/off is the signal.
 *
 * Why LEDC_CONF must be latched: the buzzer's LEDC_LOW_SPEED_MODE timer picks
 * its clock via LEDC_CONF.apb_clk_sel (1=APB, 2=RC_FAST, 3=XTAL). ledc_set_freq
 * (called from indicator_buzzer on every beep) reads that field back through
 * ledc_hal_get_clk_cfg -> ledc_ll_get_slow_clk_sel, which abort()s on any value
 * outside 1..3. ledc_timer_config writes a valid selector at init, but if that
 * write is dropped the later read returns 0 and the firmware aborts the instant
 * the first alert fires. Latching LEDC_CONF (defaulting to APB so even a read
 * before config is valid) round-trips the selector; the divider math downstream
 * may still return ESP_FAIL harmlessly (indicator_buzzer ignores the return),
 * which does not matter because on/off, not exact Hz, drives the `ind`.
 */

/* LEDC register offsets (soc/ledc_reg.h), relative to DR_REG_LEDC_BASE. The
 * buzzer is LEDC_LOW_SPEED_MODE timer 1 / channel 1 (indicators.c). */
#define R_LEDC_LSCH1_CONF0   0x0014
#define R_LEDC_LSCH1_DUTY    0x001C
#define R_LEDC_LSCH1_CONF1   0x0020
#define R_LEDC_LSTIMER1_CONF 0x00A8  /* clk_div[21:4], duty_res[3:0], para_up[25] */
#define R_LEDC_CONF          0x00D0  /* global: apb_clk_sel[1:0], clk_en[31] */

#define LEDC_CH1_SIG_OUT_EN  (1u << 2)  /* CONF0[2]: channel output enable */
/* CONF0 write-only self-clearing bits; never reported set on read-back. */
#define LEDC_CH1_PARA_UP     (1u << 4)  /* CONF0[4]  */
#define LEDC_CH1_OVF_RESET   (1u << 16) /* CONF0[16] */

/* LSTIMER1_CONF.para_up (WO, self-clearing): never reported set on read-back. */
#define LEDC_TIMER1_PARA_UP  (1u << 25)
/* Duty resolution the buzzer timer configures (LEDC_TIMER_10_BIT, indicators.c).
 * Seeded so ledc_set_freq computes a valid divider (else duty_res reads 0 and
 * the driver logs "frequency/duty cannot be achieved" on every beep). */
#define LEDC_TIMER1_DUTY_RES 10u

/* LEDC_CONF.apb_clk_sel default: APB (1) is a valid selector for
 * ledc_ll_get_slow_clk_sel, so a read before ledc_timer_config never abort()s. */
#define LEDC_CONF_APB_CLK_SEL_APB 0x1u

#define BRAMBLE_LEDC_WINDOW  0x1000

#define TYPE_BRAMBLE_LEDC "bramble.ledc"
OBJECT_DECLARE_SIMPLE_TYPE(BrambleLedcState, BRAMBLE_LEDC)

struct BrambleLedcState {
    DeviceState parent_obj;

    MemoryRegion iomem;

    uint32_t ch1_conf0;    /* LSCH1_CONF0 latched (SIG_OUT_EN is load-bearing) */
    uint32_t ch1_duty;     /* LSCH1_DUTY latched (0 = silent) */
    uint32_t timer1_conf;  /* LSTIMER1_CONF latched (duty_res must round-trip) */
    uint32_t conf;         /* LEDC_CONF latched (apb_clk_sel must round-trip) */
};

/* Recompute buzzer on/off from the latched channel-1 state and report it. */
static void bramble_ledc_eval(BrambleLedcState *s)
{
    bool on = (s->ch1_conf0 & LEDC_CH1_SIG_OUT_EN) && (s->ch1_duty != 0);
    bramble_ind_note_buzzer(on ? BRAMBLE_IND_BUZZER_HZ : 0);
}

static uint64_t bramble_ledc_read(void *opaque, hwaddr addr, unsigned int size)
{
    BrambleLedcState *s = BRAMBLE_LEDC(opaque);
    switch (addr) {
    case R_LEDC_LSCH1_CONF0:   return s->ch1_conf0;
    case R_LEDC_LSCH1_DUTY:    return s->ch1_duty;
    case R_LEDC_LSTIMER1_CONF: return s->timer1_conf;
    case R_LEDC_CONF:          return s->conf;
    default:                   return 0;
    }
}

static void bramble_ledc_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    BrambleLedcState *s = BRAMBLE_LEDC(opaque);
    uint32_t v = (uint32_t)value;
    switch (addr) {
    case R_LEDC_LSCH1_CONF0:
        /* Latch the enable; drop the WO self-clearing bits so a read-back
         * never re-asserts a commit the driver might poll to clear. */
        s->ch1_conf0 = v & ~(LEDC_CH1_PARA_UP | LEDC_CH1_OVF_RESET);
        bramble_ledc_eval(s);
        break;
    case R_LEDC_LSCH1_DUTY:
        s->ch1_duty = v;
        bramble_ledc_eval(s);
        break;
    case R_LEDC_LSCH1_CONF1:
        /* update-duty commit (duty_start); channel state already latched. */
        bramble_ledc_eval(s);
        break;
    case R_LEDC_LSTIMER1_CONF:
        /* Latch the timer config so ledc_set_freq reads back a real duty
         * resolution (mask para_up so a read never re-asserts the WO commit). */
        s->timer1_conf = v & ~LEDC_TIMER1_PARA_UP;
        break;
    case R_LEDC_CONF:
        /* Latch the global config so ledc_set_freq reads back the clock
         * selector ledc_timer_config wrote (else apb_clk_sel==0 -> abort). */
        s->conf = v;
        break;
    default:
        break;
    }
}

static const MemoryRegionOps bramble_ledc_ops = {
    .read = bramble_ledc_read,
    .write = bramble_ledc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void bramble_ledc_instance_init(Object *obj)
{
    BrambleLedcState *s = BRAMBLE_LEDC(obj);

    /* Seed a valid clock selector so a read before ledc_timer_config never
     * hits the ledc_ll_get_slow_clk_sel abort(), and a plausible duty
     * resolution so the divider math is valid from the first beep. */
    s->conf = LEDC_CONF_APB_CLK_SEL_APB;
    s->timer1_conf = LEDC_TIMER1_DUTY_RES;

    memory_region_init_io(&s->iomem, obj, &bramble_ledc_ops, s,
                          TYPE_BRAMBLE_LEDC, BRAMBLE_LEDC_WINDOW);
}

static const TypeInfo bramble_ledc_info = {
    .name = TYPE_BRAMBLE_LEDC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(BrambleLedcState),
    .instance_init = bramble_ledc_instance_init,
};

/* Overlay the LEDC window at higher priority than the machine's catch-all IO
 * region (added at priority 0), like bramble_gpio / bramble_adc. */
static void bramble_ledc_attach(MemoryRegion *sys_mem)
{
    Object *obj = object_new(TYPE_BRAMBLE_LEDC);
    BrambleLedcState *s = BRAMBLE_LEDC(obj);
    bramble_overlay_attach(obj, "bramble-ledc", &s->iomem, sys_mem,
                           DR_REG_LEDC_BASE,
                           "bramble-ledc: buzzer tone overlay");
}

/* ---- SSD1680 e-paper SSI slave (P2.5) ------------------------------------ */
/*
 * Register-accurate model of the Solomon SSD1680 controller driving the pager's
 * GDEY0213B74 2.13" e-paper (components/display/ssd1680_io.c on top of the
 * shared ssd1680_engine.c). It replaces the P2.4 0x00 display stub: it decodes
 * the command/data stream the firmware clocks out, rebuilds the controller's
 * image RAM, and on Master Activation (0x20) unpacks that RAM into the SAME
 * 250x122 1bpp logical framebuffer the linux node ships (display_virt.c) and
 * emits it to the gosim ether as an emu-link `fb` message. The browser device
 * view then renders the QEMU pager's screen pixel-identical to a linux node.
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

#define TYPE_BRAMBLE_SSD1680 "bramble.ssd1680"
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

/* ---- SX1262 LoRa radio SSI slave (P2.4a) --------------------------------- */
/*
 * Register-accurate model of the Semtech SX1262 the pager's radio driver
 * (components/radio/sx1262.c) talks to over SPI2. It replaces the 0x00 stub for
 * radio-routed transfers so radio_init's command stream is answered correctly,
 * register/buffer reads round-trip, and P2.4b can layer emu-link TX/RX + DIO1
 * IRQ on the state kept here.
 *
 * SX1262 SPI framing (datasheet section 13): every transfer is CS-low,
 * [opcode][params/data...], CS-high. The chip returns a status byte on the
 * bytes clocked AFTER the opcode; command data follows. A per-transaction byte
 * cursor (reset by set_cs on CS assert) decodes each opcode the driver issues:
 *
 *   WriteRegister 0x0D  [addrH][addrL][data..]      -> latch into reg file
 *   ReadRegister  0x1D  [addrH][addrL][NOP][data..] -> status then reg file
 *   WriteBuffer   0x0E  [offset][data..]            -> latch into data buffer
 *   ReadBuffer    0x1E  [offset][NOP][data..]       -> status then data buffer
 *   GetStatus     0xC0  [status]                    -> status byte
 *   GetIrqStatus  0x12  [status][irqH][irqL]        -> pending IRQ flags
 *   GetRxBufStatus 0x13 / GetPacketStatus 0x14      -> status then zeros
 *   Set* / config commands                          -> accepted, mode latched
 *
 * BUSY (GPIO13) is served low by the P2.2 overlay; DIO1 (RX/TX-done IRQ) is not
 * asserted here (over-the-air activity is P2.4b).
 */

/* SX1262 SPI op-codes (mirror components/radio/include/sx1262.h). */
#define SX1262_CMD_SET_SLEEP        0x84
#define SX1262_CMD_SET_STANDBY      0x80
#define SX1262_CMD_SET_FS           0xC1
#define SX1262_CMD_SET_TX           0x83
#define SX1262_CMD_SET_RX           0x82
#define SX1262_CMD_SET_CAD          0xC5
#define SX1262_CMD_GET_STATUS       0xC0
#define SX1262_CMD_WRITE_REGISTER   0x0D
#define SX1262_CMD_READ_REGISTER    0x1D
#define SX1262_CMD_WRITE_BUFFER     0x0E
#define SX1262_CMD_READ_BUFFER      0x1E
#define SX1262_CMD_GET_IRQ_STATUS   0x12
#define SX1262_CMD_GET_RX_BUFF_STATUS 0x13
#define SX1262_CMD_GET_PKT_STATUS   0x14
#define SX1262_CMD_CLR_IRQ_STATUS   0x02
#define SX1262_CMD_SET_RF_FREQ      0x86
#define SX1262_CMD_SET_MOD_PARAMS   0x8B
#define SX1262_CMD_SET_PKT_PARAMS   0x8C
#define SX1262_CMD_SET_TX_PARAMS    0x8E

/* IRQ-status bits the radio driver reads via GetIrqStatus (mirror sx1262.h). */
#define SX1262_IRQ_TX_DONE          (1u << 0)
#define SX1262_IRQ_RX_DONE          (1u << 1)

/* DIO1 = GPIO14 (main/boards/bramble_pager.h): the radio driver installs a
 * posedge ISR here; the SX1262 model raises it on TxDone/RxDone via the GPIO
 * overlay's input accessor (P2.4b). */
#define SX1262_DIO1_GPIO            14

/* Radio manual software chip-select = GPIO8 (sx1262.c sets spics_io_num = -1 and
 * toggles gpio_set_level(8) by hand); GPIO8 low selects the SX1262 in the
 * controller's CS routing (bramble_gpspi2_route). */
#define SX1262_CS_GPIO              8

/* Status byte (datasheet 13.5.1): [6:4]=chip mode, [3:1]=command status. */
#define SX1262_MODE_STDBY_RC        0x2
#define SX1262_MODE_FS              0x4
#define SX1262_MODE_RX              0x5
#define SX1262_MODE_TX              0x6
#define SX1262_CMD_STATUS_OK        0x2  /* benign non-error; init never checks */

#define SX1262_REGFILE_SIZE         0x1000 /* covers OCP 0x08E7, sync 0x0740 */
#define SX1262_BUFFER_SIZE          0x100  /* 256-byte TX/RX data buffer */

/* Bounded FIFO for frames the broker delivers faster than the slow guest
 * drains them (P2.4c). The radio driver's radio_task drains exactly ONE frame
 * per DIO1 rising edge - GetIrqStatus -> ClrIrqStatus -> GetRxBufferStatus ->
 * ReadBuffer -> GetPacketStatus, then back to ulTaskNotifyTake - so overlapping
 * arrivals must NOT overwrite the live RX buffer or collapse onto one edge:
 * each queues here and is presented in turn, one fresh 0->1 DIO1 edge per
 * frame. Bounded; oldest dropped (logged) on overflow so loss is never silent. */
#define SX1262_RX_FIFO_DEPTH        8

typedef struct {
    uint8_t data[255];  /* LoRa payload, capped at the SX1262 255-byte max */
    uint8_t len;
    uint8_t rssi_raw;   /* GetPacketStatus byte 0 encoding: rssi = -raw/2 */
    int8_t snr_raw;     /* GetPacketStatus byte 1 encoding: snr = raw/4 */
} Sx1262RxFrame;

#define TYPE_BRAMBLE_SX1262 "bramble.sx1262"
OBJECT_DECLARE_SIMPLE_TYPE(BrambleSx1262State, BRAMBLE_SX1262)

struct BrambleSx1262State {
    SSIPeripheral parent_obj;

    /* Persistent chip state (survives across transactions). */
    uint8_t regs[SX1262_REGFILE_SIZE];
    uint8_t buffer[SX1262_BUFFER_SIZE];
    uint8_t mode;          /* current chip mode (SX1262_MODE_*) */
    uint16_t irq_status;   /* pending IRQ flags (driven by emu-link txdone/rx) */

    /* PHY params latched from the driver's config commands, emitted with each
     * `tx` so the broker/UI see the transmit parameters (P2.4b). */
    int tx_freq_mhz;       /* SetRfFrequency (0x86), decoded to MHz */
    int tx_sf;             /* SetModulationParams (0x8B) spreading factor */
    int tx_bw_hz;          /* SetModulationParams bandwidth, Hz */
    int tx_cr;             /* SetModulationParams coding rate */
    int tx_power;          /* SetTxParams (0x8E) power, dBm */
    uint8_t tx_len;        /* SetPacketParams (0x8C) payload length */
    uint32_t freq_raw;     /* accumulator for the 4-byte SetRfFrequency value */

    /* RX state for the frame CURRENTLY presented to the driver, read back via
     * GetRxBufferStatus (0x13) / GetPacketStatus (0x14). Held stable across the
     * whole drain sequence (the driver ReadBuffers AFTER it ClrIrqStatus-es). */
    uint8_t rx_len;
    uint8_t rx_offset;
    uint8_t rssi_raw;      /* GetPacketStatus byte 0: rssi = -raw/2 */
    int8_t snr_raw;        /* GetPacketStatus byte 1: snr = raw/4 */

    /* RX frame handling (P2.4c). The presented frame is held in rx_cur, SEPARATE
     * from the FIFO of not-yet-presented frames, so a TX that clobbers the SPI
     * buffer mid-flight can reload it and the FIFO never loses it. rx_active ==
     * a frame is latched awaiting the driver's drain; dio1_level mirrors the
     * DIO1 (GPIO14) line the model drives, so SetRx can tell a still-asserted
     * present from one the driver cleared without draining and re-arm the edge. */
    Sx1262RxFrame rx_fifo[SX1262_RX_FIFO_DEPTH];
    int rx_fifo_head;      /* index of the oldest queued (un-presented) frame */
    int rx_fifo_count;     /* number of frames waiting in the FIFO */
    Sx1262RxFrame rx_cur;  /* the frame currently presented to the driver */
    bool rx_active;        /* rx_cur is latched, driver has not finished draining */
    bool dio1_level;       /* model's view of the DIO1/GPIO14 line it drives */

    /* Per-transaction cursor, reset on CS assert. */
    uint32_t byte_idx;     /* bytes seen since CS went low (opcode == 0) */
    uint8_t opcode;        /* latched at byte 0 */
    uint16_t reg_addr;     /* running address for Read/WriteRegister */
    uint8_t buf_offset;    /* running offset for Read/WriteBuffer */
    uint16_t clr_mask;     /* accumulator for the 2-byte ClrIrqStatus value */
};

static uint8_t bramble_sx1262_status(BrambleSx1262State *s)
{
    return (uint8_t)((s->mode << 4) | (SX1262_CMD_STATUS_OK << 1));
}

/* CS transition. ssi_cs_default passes the raw line level; SSI_CS_LOW means the
 * chip is selected while the line is low, so a low level starts a fresh
 * transaction (reset the byte cursor). */
static int bramble_sx1262_set_cs(SSIPeripheral *dev, bool level)
{
    BrambleSx1262State *s = BRAMBLE_SX1262(dev);
    if (!level) {
        s->byte_idx = 0;
    }
    return 0;
}

/* Drive the DIO1 (GPIO14) line and remember its level. The GPIO overlay latches
 * a status edge only on a genuine 0->1 transition, so tracking the level here
 * lets the RX re-arm (SetRx) tell an asserted present apart from one the driver
 * has already cleared. */
static void bramble_sx1262_set_dio1(BrambleSx1262State *s, bool level)
{
    s->dio1_level = level;
    bramble_gpio_set_input(SX1262_DIO1_GPIO, level);
}

/* Broker `txdone`{toa_ms}: the airtime the broker priced has elapsed on the sim
 * clock. Latch TX_DONE and raise DIO1 so the driver's posedge ISR wakes the
 * radio task, which reads GetIrqStatus (TX_DONE) and unblocks radio_transmit_raw
 * (radio_esp.c). Runs on the QEMU main loop under the BQL. */
static void bramble_sx1262_on_txdone(QDict *msg, void *ctx)
{
    (void)msg;
    BrambleSx1262State *s = ctx;
    s->irq_status |= SX1262_IRQ_TX_DONE;
    bramble_sx1262_set_dio1(s, true);
}

/* Copy the currently presented frame (rx_cur) into the SPI-readable buffer +
 * packet-status fields. Called on present and, defensively, on RX re-arm in
 * case a TX's WriteBuffer clobbered the buffer while the frame was pending. */
static void bramble_sx1262_rx_load_cur(BrambleSx1262State *s)
{
    memcpy(s->buffer, s->rx_cur.data, s->rx_cur.len);
    s->rx_len = s->rx_cur.len;
    s->rx_offset = 0;
    s->rssi_raw = s->rx_cur.rssi_raw;
    s->snr_raw = s->rx_cur.snr_raw;
}

/* Pop the oldest queued frame into rx_cur, latch RX_DONE, and raise a fresh DIO1
 * edge for the driver's posedge ISR. Called when the link delivers a frame with
 * none in flight, and again each time the driver finishes draining the previous
 * one. No-op if the FIFO is empty or a frame is already latched. */
static void bramble_sx1262_rx_present(BrambleSx1262State *s)
{
    /* Only present (assert DIO1) while the chip is actually in RX: a real
     * SX1262 does not raise RX_DONE in standby. Frames the broker delivers
     * during radio_init (mode still STDBY) queue silently and are presented by
     * SetRx's re-arm once the driver starts listening; asserting DIO1 mid-init
     * would wake the just-installed ISR to do GetIrqStatus SPI interleaved with
     * radio_init's own config SPI and stall the init sequence. */
    if (s->mode != SX1262_MODE_RX) {
        return;
    }
    if (s->rx_active || s->rx_fifo_count == 0) {
        return;
    }
    s->rx_cur = s->rx_fifo[s->rx_fifo_head];
    s->rx_fifo_head = (s->rx_fifo_head + 1) % SX1262_RX_FIFO_DEPTH;
    s->rx_fifo_count--;

    bramble_sx1262_rx_load_cur(s);
    s->irq_status |= SX1262_IRQ_RX_DONE;
    s->rx_active = true;
    /* DIO1 was driven low by the previous frame's ClrIrqStatus (or was never
     * raised), so this is a real 0->1 edge that fires the driver's ISR. */
    bramble_sx1262_set_dio1(s, true);
}

/* The driver has clocked out GetPacketStatus, radio_task's LAST RX-drain SPI op
 * (GetIrqStatus -> ClrIrqStatus -> GetRxBufferStatus -> ReadBuffer ->
 * GetPacketStatus). rx_cur is fully consumed, so drop the latch and hand over
 * the next queued frame with its own DIO1 edge. This is the correct swap point
 * precisely BECAUSE the driver ClrIrqStatus-es before it ReadBuffers:
 * presenting the next frame any earlier (e.g. on the clear) would overwrite the
 * live buffer under the imminent read. */
static void bramble_sx1262_rx_drain_done(BrambleSx1262State *s)
{
    if (!s->rx_active) {
        return;
    }
    s->rx_active = false;
    bramble_sx1262_rx_present(s);
}

/* SetRx (0x82): the driver (re-)enters continuous RX. This is the universal
 * re-arm point - boot (radio_init -> radio_start_rx, AFTER the DIO1 ISR is
 * installed) and after every TX (radio_transmit_raw -> radio_start_rx). Frames
 * the broker delivered while the guest was still booting were presented before
 * the driver could catch the edge, then radio_start_rx's ClrIrqStatus dropped
 * DIO1 without a drain; without this the latch would stick forever and the FIFO
 * back up (observed in P2.4b). On re-arm: if a frame is still pending but DIO1
 * is low, reload it (a TX may have clobbered the buffer) and raise a fresh edge;
 * otherwise present the head of the FIFO if one is waiting. */
static void bramble_sx1262_rx_rearm(BrambleSx1262State *s)
{
    if (s->rx_active) {
        if (!s->dio1_level) {
            bramble_sx1262_rx_load_cur(s);
            s->irq_status |= SX1262_IRQ_RX_DONE;
            bramble_sx1262_set_dio1(s, true);
        }
    } else {
        bramble_sx1262_rx_present(s);
    }
}

/* Broker `rx`{payload,rssi,snr,freq}: a frame survived the ether's
 * collision/capture model. Decode + queue it; if nothing is currently latched,
 * present it immediately (RX_DONE + DIO1 edge). The driver's ISR then reads
 * GetIrqStatus (RX_DONE), GetRxBufferStatus (len/offset), ReadBuffer, and
 * GetPacketStatus (rssi/snr), exactly like the real chip, and the FIFO feeds it
 * the next frame on drain so back-to-back arrivals each get their own edge
 * instead of overwriting the buffer under a stuck-high DIO1. */
static void bramble_sx1262_on_rx(QDict *msg, void *ctx)
{
    BrambleSx1262State *s = ctx;
    const char *payload = qdict_get_try_str(msg, "payload");
    if (!payload) {
        return;
    }
    size_t n = 0;
    g_autofree uint8_t *decoded = qbase64_decode(payload, -1, &n, NULL);
    if (!decoded || n == 0 || n > sizeof(s->rx_fifo[0].data)) {
        return; /* radio frames are never empty and cap at 255 bytes */
    }

    int rssi = (int)qdict_get_try_int(msg, "rssi", -100);
    int snr = (int)qdict_get_try_int(msg, "snr", 0);
    /* GetPacketStatus readback encoding (sx1262.c): rssi = -raw/2, snr = raw/4. */
    int rssi_raw = -2 * rssi;
    if (rssi_raw < 0) rssi_raw = 0;
    if (rssi_raw > 255) rssi_raw = 255;
    int snr_raw = 4 * snr;
    if (snr_raw < -128) snr_raw = -128;
    if (snr_raw > 127) snr_raw = 127;

    /* Full FIFO: drop the oldest un-presented frame so the newest still lands,
     * and say so (the brief's "no silent loss"). The presented frame lives in
     * rx_cur, not the FIFO, so it is never the one dropped. */
    if (s->rx_fifo_count == SX1262_RX_FIFO_DEPTH) {
        fprintf(stderr,
                "bramble-sx1262: RX FIFO full (%d), dropping oldest frame\n",
                SX1262_RX_FIFO_DEPTH);
        s->rx_fifo_head = (s->rx_fifo_head + 1) % SX1262_RX_FIFO_DEPTH;
        s->rx_fifo_count--;
    }
    int tail = (s->rx_fifo_head + s->rx_fifo_count) % SX1262_RX_FIFO_DEPTH;
    Sx1262RxFrame *f = &s->rx_fifo[tail];
    memcpy(f->data, decoded, n);
    f->len = (uint8_t)n;
    f->rssi_raw = (uint8_t)rssi_raw;
    f->snr_raw = (int8_t)snr_raw;
    s->rx_fifo_count++;

    bramble_sx1262_rx_present(s);
}

static uint32_t bramble_sx1262_transfer(SSIPeripheral *dev, uint32_t val)
{
    BrambleSx1262State *s = BRAMBLE_SX1262(dev);
    uint8_t in = val & 0xff;
    uint32_t idx = s->byte_idx++;
    uint8_t out = bramble_sx1262_status(s);

    /* Byte 0 is always the opcode; the chip clocks out its status meanwhile. */
    if (idx == 0) {
        s->opcode = in;
        s->reg_addr = 0;
        s->buf_offset = 0;
        switch (in) { /* mode-changing commands take effect for status. */
        case SX1262_CMD_SET_FS:  s->mode = SX1262_MODE_FS; break;
        case SX1262_CMD_SET_TX:
            s->mode = SX1262_MODE_TX;
            /* The driver has already written the frame (WriteBuffer @0) and
             * latched the length/PHY params in the preceding transactions;
             * SetTx is the "key the channel" trigger. Emit it onto the ether.
             * The broker prices airtime and replies `txdone`, which raises
             * DIO1 (bramble_sx1262_on_txdone). If no broker is wired the send
             * no-ops and radio_transmit_raw falls through to its timeout. */
            if (s->tx_len > 0) {
                emulink_send_tx(s->buffer, s->tx_len, s->tx_freq_mhz,
                                        s->tx_sf, s->tx_bw_hz, s->tx_cr,
                                        s->tx_power);
            }
            break;
        case SX1262_CMD_SET_RX:
            s->mode = SX1262_MODE_RX;
            /* Re-arm RX: hand over a frame that arrived while the guest was not
             * yet listening, or re-raise a pending one the driver cleared
             * without draining (see bramble_sx1262_rx_rearm). */
            bramble_sx1262_rx_rearm(s);
            break;
        case SX1262_CMD_SET_CAD: s->mode = SX1262_MODE_RX; break;
        case SX1262_CMD_SET_SLEEP:
        case SX1262_CMD_SET_STANDBY:
        default: s->mode = SX1262_MODE_STDBY_RC; break;
        }
        return bramble_sx1262_status(s);
    }

    switch (s->opcode) {
    case SX1262_CMD_WRITE_REGISTER:
        if (idx == 1) {
            s->reg_addr = (uint16_t)in << 8;
        } else if (idx == 2) {
            s->reg_addr |= in;
        } else {
            s->regs[s->reg_addr % SX1262_REGFILE_SIZE] = in;
            s->reg_addr++;
        }
        break;
    case SX1262_CMD_READ_REGISTER:
        if (idx == 1) {
            s->reg_addr = (uint16_t)in << 8;
        } else if (idx == 2) {
            s->reg_addr |= in;
        } else if (idx == 3) {
            /* NOP byte: chip returns status. */
        } else {
            out = s->regs[s->reg_addr % SX1262_REGFILE_SIZE];
            s->reg_addr++;
        }
        break;
    case SX1262_CMD_WRITE_BUFFER:
        if (idx == 1) {
            s->buf_offset = in;
        } else {
            s->buffer[s->buf_offset++] = in;
        }
        break;
    case SX1262_CMD_READ_BUFFER:
        if (idx == 1) {
            s->buf_offset = in;
        } else if (idx == 2) {
            /* NOP byte: chip returns status. */
        } else {
            out = s->buffer[s->buf_offset++];
        }
        break;
    case SX1262_CMD_GET_IRQ_STATUS:
        if (idx == 2) {
            out = (s->irq_status >> 8) & 0xff;
        } else if (idx == 3) {
            out = s->irq_status & 0xff;
        }
        break;
    case SX1262_CMD_CLR_IRQ_STATUS:
        /* [maskH][maskL]: clear the masked IRQ bits and drop DIO1 so the next
         * TxDone/RxDone is a fresh 0->1 edge. The driver clears here BEFORE it
         * ReadBuffers, so the presented frame stays latched (rx_active) and is
         * only swapped out once GetPacketStatus completes the drain; see
         * bramble_sx1262_rx_drain_done. */
        if (idx == 1) {
            s->clr_mask = (uint16_t)in << 8;
        } else if (idx == 2) {
            s->clr_mask |= in;
            s->irq_status &= ~s->clr_mask;
            bramble_sx1262_set_dio1(s, false);
        }
        break;
    case SX1262_CMD_GET_RX_BUFF_STATUS:
        /* status, then payload length, then rx start offset. */
        if (idx == 2) {
            out = s->rx_len;
        } else if (idx == 3) {
            out = s->rx_offset;
        }
        break;
    case SX1262_CMD_GET_PKT_STATUS:
        /* status, then rssi_raw, snr_raw, signal_rssi. This is radio_task's
         * final RX-drain op: on its last byte the current frame is fully
         * consumed, so present the next queued frame (a fresh DIO1 edge). */
        if (idx == 2) {
            out = s->rssi_raw;
        } else if (idx == 3) {
            out = (uint8_t)s->snr_raw;
        } else {
            out = s->rssi_raw; /* signal_rssi (unused by the firmware) */
            if (idx == 4) {
                bramble_sx1262_rx_drain_done(s);
            }
        }
        break;
    case SX1262_CMD_GET_STATUS:
        /* Byte 1 already returned status; later bytes read 0. */
        break;
    case SX1262_CMD_SET_RF_FREQ:
        /* 4 big-endian bytes of freq_raw; decode to MHz on the last one.
         * freq_hz = freq_raw * 32e6 / 2^25. */
        if (idx == 1) {
            s->freq_raw = (uint32_t)in << 24;
        } else if (idx == 2) {
            s->freq_raw |= (uint32_t)in << 16;
        } else if (idx == 3) {
            s->freq_raw |= (uint32_t)in << 8;
        } else if (idx == 4) {
            s->freq_raw |= in;
            s->tx_freq_mhz =
                (int)(((double)s->freq_raw * 32.0 / 33554432.0) + 0.5);
        }
        break;
    case SX1262_CMD_SET_MOD_PARAMS:
        /* [sf][bw_param][cr][ldro]. */
        if (idx == 1) {
            s->tx_sf = in;
        } else if (idx == 2) {
            s->tx_bw_hz = (in == 0x05) ? 250000 : (in == 0x06) ? 500000 : 125000;
        } else if (idx == 3) {
            s->tx_cr = in;
        }
        break;
    case SX1262_CMD_SET_PKT_PARAMS:
        /* [preH][preL][hdr][payload_len][crc][iq]: latch the TX length. */
        if (idx == 4) {
            s->tx_len = in;
        }
        break;
    case SX1262_CMD_SET_TX_PARAMS:
        /* [power][ramp_time]. */
        if (idx == 1) {
            s->tx_power = (int8_t)in;
        }
        break;
    default:
        /* All other Set/config commands: accept the parameter bytes. */
        break;
    }
    return out;
}

static void bramble_sx1262_realize(SSIPeripheral *dev, Error **errp)
{
    BrambleSx1262State *s = BRAMBLE_SX1262(dev);
    (void)errp;
    s->mode = SX1262_MODE_STDBY_RC;
    s->irq_status = 0;
    s->byte_idx = 0;
    s->rx_fifo_head = 0;
    s->rx_fifo_count = 0;
    s->rx_active = false;
    s->dio1_level = false;

    /* Sensible PHY defaults in case SetTx precedes a full config (it never does
     * on the real driver, which configures freq/mod/packet params first). */
    s->tx_freq_mhz = 915;
    s->tx_sf = 7;
    s->tx_bw_hz = 125000;
    s->tx_cr = 1;
    s->tx_power = 22;

    /* Register for the broker's over-the-air events. The handler table is
     * static and independent of chardev attach order (bramble_emulink.c), so
     * registering here at realize is safe even though the chardev is wired
     * later at machine init. */
    emulink_on("txdone", bramble_sx1262_on_txdone, s);
    emulink_on("rx", bramble_sx1262_on_rx, s);
}

static void bramble_sx1262_class_init(ObjectClass *klass, void *data)
{
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    k->realize = bramble_sx1262_realize;
    k->transfer = bramble_sx1262_transfer;
    k->set_cs = bramble_sx1262_set_cs;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo bramble_sx1262_info = {
    .name = TYPE_BRAMBLE_SX1262,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(BrambleSx1262State),
    .class_init = bramble_sx1262_class_init,
};

/* ---- GPSPI2 controller --------------------------------------------------- */

#define TYPE_BRAMBLE_GPSPI2 "bramble.gpspi2"
OBJECT_DECLARE_SIMPLE_TYPE(BrambleGpspi2State, BRAMBLE_GPSPI2)

struct BrambleGpspi2State {
    DeviceState parent_obj;

    MemoryRegion iomem;
    SSIBus *spi;

    /* SSI_GPIO_CS inputs of the two bus slaves, driven per-transfer from the
     * CS-routing decision (bramble_gpspi2_route). radio_cs -> SX1262 slave,
     * disp_cs -> SSD1680 display slave. */
    qemu_irq radio_cs;
    qemu_irq disp_cs;

    /* Latched configuration registers the transaction reads. */
    uint32_t addr;
    uint32_t ctrl;
    uint32_t clock;
    uint32_t user;
    uint32_t user1;
    uint32_t user2;
    uint32_t ms_dlen;
    uint32_t misc;
    uint32_t dma_conf;
    uint32_t dma_int_ena;
    uint32_t dma_int_raw;
    uint32_t slave;
    uint32_t slave1;
    uint32_t clk_gate;

    /* SPI_W0..W15 CPU data buffer (little-endian byte order on the wire). */
    uint32_t data_reg[GPSPI2_BUF_WORDS];

    /* GDMA controller for the DMA data path (SPI2 channel), and the SPI2
     * transfer-done interrupt line (unused by the polling firmware). */
    ESPGdmaState *gdma;
    qemu_irq intr;
};

/* SPI2 transfer-done is a LEVEL interrupt (esp32s3_intc.h marks the source
 * "level", and the intmatrix forwards the line straight to the Xtensa external
 * interrupt). Assert the line whenever an enabled interrupt is raw-pending and
 * hold it until the guest ISR clears the raw bit via SPI_DMA_INT_CLR.
 *
 * This matters because the pager mixes two SPI driver paths: the e-paper polls
 * SPI_USR (spi_device_polling_transmit) and never enables this interrupt, but
 * the RADIO (sx1262.c) uses the interrupt-driven spi_device_transmit path and
 * blocks in spi_device_get_trans_result waiting for this very interrupt. A
 * momentary pulse (raise then immediately lower, inside the SPI_USR MMIO write)
 * is sampled by the CPU only after the level has already dropped, so it is lost
 * and radio_init wedges forever. Driving a real level that stays asserted until
 * the ISR acknowledges is what lets the interrupt-driven transfer complete. */
static void bramble_gpspi2_update_irq(BrambleGpspi2State *s)
{
    if (s->intr) {
        qemu_set_irq(s->intr, (s->dma_int_raw & s->dma_int_ena) ? 1 : 0);
    }
}

/* Select the bus slave this transfer targets. The radio's manual chip select is
 * GPIO8 (driven by sx1262.c via gpio_set_level and observed by the P2.2
 * overlay): GPIO8 low selects the SX1262 slave. The display uses hardware CS0
 * (it is SPI device 0, added by display_init before radio_init), which the IDF
 * spi_master enables per transaction via SPI_MISC_REG.CS0_DIS; it is selected
 * POSITIVELY when CS0 is enabled and the radio's soft CS is not asserting. Both
 * slaves are SSI_CS_LOW, so a 0 on their SSI_GPIO_CS input means selected.
 * `active` gates whether either is selected (deasserted between transfers so the
 * next assert re-triggers the SX1262's set_cs byte-cursor reset). */
static void bramble_gpspi2_route(BrambleGpspi2State *s, int active)
{
    if (!active) {
        qemu_set_irq(s->radio_cs, 1);
        qemu_set_irq(s->disp_cs, 1);
        return;
    }
    bool radio_sel = (bramble_gpio_out_level(SX1262_CS_GPIO) == 0);
    bool disp_sel = !radio_sel && !(s->misc & SPI_MISC_CS0_DIS);
    qemu_set_irq(s->radio_cs, radio_sel ? 0 : 1);
    qemu_set_irq(s->disp_cs, disp_sel ? 0 : 1);
}

/* Shift `nbytes` low bytes of `value` out, LSB-first, discarding MISO. Used for
 * the (rarely exercised on the pager) peripheral command/address phases. */
static void bramble_gpspi2_shift_scalar(BrambleGpspi2State *s, uint32_t value,
                                        uint32_t nbytes)
{
    for (uint32_t i = 0; i < nbytes; i++) {
        ssi_transfer(s->spi, (value >> (8 * i)) & 0xff);
    }
}

/* Perform a full user transaction: CS assert, optional command/addr/dummy
 * phases, then the MOSI/MISO data phase, then CS deassert. The data phase
 * sources TX and sinks RX from either the CPU W-buffer or the SPI2 GDMA channel
 * depending on the per-transfer DMA-enable bits. */
static void bramble_gpspi2_transfer(BrambleGpspi2State *s)
{
    const bool do_cmd   = s->user & SPI_USER_COMMAND;
    const bool do_addr  = s->user & SPI_USER_ADDR;
    const bool do_mosi  = s->user & SPI_USER_MOSI;
    const bool do_miso  = s->user & SPI_USER_MISO;
    const bool dma_tx   = s->dma_conf & SPI_DMA_TX_ENA;
    const bool dma_rx   = s->dma_conf & SPI_DMA_RX_ENA;

    /* Data phase length: SPI_MS_DATA_BITLEN holds (bits - 1). */
    uint32_t data_bits = s->ms_dlen & SPI_MS_DATA_BITLEN_MASK;
    uint32_t data_bytes = (do_mosi || do_miso) ? (data_bits + 1) / 8 : 0;

    /* Route to the SX1262 (GPIO8 low) or the display stub before any byte is
     * shifted, so the selected slave sees this whole transaction. */
    bramble_gpspi2_route(s, 1);

    if (do_cmd) {
        uint32_t cmd_val = s->user2 & SPI_USER2_CMD_VALUE_MASK;
        uint32_t cmd_bits =
            ((s->user2 >> SPI_USER2_CMD_BITLEN_SHIFT) & SPI_USER2_CMD_BITLEN_MASK) + 1;
        bramble_gpspi2_shift_scalar(s, cmd_val, (cmd_bits + 7) / 8);
    }
    if (do_addr) {
        bramble_gpspi2_shift_scalar(s, s->addr, 4);
    }

    if (data_bytes) {
        /* Shift through stack buffers for the common case; spill to the heap
         * only for an oversized transfer the pager never issues. Neither buffer
         * is pre-zeroed (the old g_malloc0 was fully wasted on the hot path):
         * rxbuf is completely written by the shift loop, and txbuf is filled
         * below - with an explicit tail-zero on the CPU path past the 64-byte
         * W-buffer, and on a missed DMA fetch, to preserve the g_malloc0
         * semantics the register-accurate slaves may depend on. */
        uint8_t txstack[GPSPI2_MAX_XFER];
        uint8_t rxstack[GPSPI2_MAX_XFER];
        g_autofree uint8_t *txheap = NULL;
        g_autofree uint8_t *rxheap = NULL;
        uint8_t *txbuf = txstack;
        uint8_t *rxbuf = rxstack;
        if (data_bytes > GPSPI2_MAX_XFER) {
            txbuf = txheap = g_malloc(data_bytes);
            rxbuf = rxheap = g_malloc(data_bytes);
        }

        if (do_mosi) {
            if (dma_tx && s->gdma) {
                uint32_t chan;
                if (!esp_gdma_get_channel_periph(s->gdma, GDMA_SPI2,
                                                 ESP_GDMA_OUT_IDX, &chan) ||
                    !esp_gdma_read_channel(s->gdma, chan, txbuf, data_bytes)) {
                    /* Best-effort: a stub slave discards MOSI, so a missed DMA
                     * fetch does not wedge boot. Zero-fill to match the old
                     * g_malloc0 (a register-accurate slave then sees zeros, not
                     * stale stack bytes). Correct framebuffer bytes are a P2.5
                     * concern (see channel-disambiguation note below). */
                    memset(txbuf, 0, data_bytes);
                    qemu_log_mask(LOG_UNIMP,
                        "bramble-gpspi2: SPI2 GDMA OUT fetch failed (%u bytes)\n",
                        data_bytes);
                }
            } else {
                uint32_t ncpu = MIN(data_bytes, GPSPI2_BUF_BYTES);
                memcpy(txbuf, s->data_reg, ncpu);
                if (data_bytes > ncpu) {
                    /* CPU W-buffer past 64 bytes reads back as 0. */
                    memset(txbuf + ncpu, 0, data_bytes - ncpu);
                }
            }
        }

        for (uint32_t i = 0; i < data_bytes; i++) {
            uint8_t out = do_mosi ? txbuf[i] : 0xff;
            rxbuf[i] = ssi_transfer(s->spi, out) & 0xff;
        }

        if (do_miso) {
            if (dma_rx && s->gdma) {
                uint32_t chan;
                if (!esp_gdma_get_channel_periph(s->gdma, GDMA_SPI2,
                                                 ESP_GDMA_IN_IDX, &chan) ||
                    !esp_gdma_write_channel(s->gdma, chan, rxbuf, data_bytes)) {
                    qemu_log_mask(LOG_UNIMP,
                        "bramble-gpspi2: SPI2 GDMA IN push failed (%u bytes)\n",
                        data_bytes);
                }
            } else {
                memcpy(s->data_reg, rxbuf, MIN(data_bytes, GPSPI2_BUF_BYTES));
            }
        }
    }

    bramble_gpspi2_route(s, 0);

    /* Latch transfer-done and drive the (level) interrupt line. The e-paper
     * polls SPI_USR and leaves the interrupt disabled, so this is a no-op for
     * it; the radio enables it and the held level is what wakes its blocked
     * spi_device_transmit. See bramble_gpspi2_update_irq. */
    s->dma_int_raw |= SPI_TRANS_DONE_INT;
    bramble_gpspi2_update_irq(s);
}

static uint64_t bramble_gpspi2_read(void *opaque, hwaddr addr, unsigned int size)
{
    BrambleGpspi2State *s = BRAMBLE_GPSPI2(opaque);
    switch (addr) {
    /* SPI_USR / SPI_UPDATE are self-clearing: the transaction runs
     * synchronously on write, so both always read back 0 ("done"). */
    case R_SPI_CMD:          return 0;
    case R_SPI_ADDR:         return s->addr;
    case R_SPI_CTRL:         return s->ctrl;
    case R_SPI_CLOCK:        return s->clock;
    case R_SPI_USER:         return s->user;
    case R_SPI_USER1:        return s->user1;
    case R_SPI_USER2:        return s->user2;
    case R_SPI_MS_DLEN:      return s->ms_dlen;
    case R_SPI_MISC:         return s->misc;
    case R_SPI_DMA_CONF:     return s->dma_conf;
    case R_SPI_DMA_INT_ENA:  return s->dma_int_ena;
    case R_SPI_DMA_INT_RAW:  return s->dma_int_raw;
    case R_SPI_DMA_INT_ST:   return s->dma_int_raw & s->dma_int_ena;
    case R_SPI_SLAVE:        return s->slave;
    case R_SPI_SLAVE1:       return s->slave1;
    case R_SPI_CLK_GATE:     return s->clk_gate;
    case R_SPI_W0 ... R_SPI_W15:
        return s->data_reg[(addr - R_SPI_W0) / sizeof(uint32_t)];
    default:                 return 0;
    }
}

static void bramble_gpspi2_write(void *opaque, hwaddr addr, uint64_t value,
                                 unsigned int size)
{
    BrambleGpspi2State *s = BRAMBLE_GPSPI2(opaque);
    uint32_t v = (uint32_t)value;
    switch (addr) {
    case R_SPI_CMD:
        /* SPI_UPDATE is a no-op sync bit (reads back 0). SPI_USR kicks the
         * transaction; we run it here and leave the bit clear. */
        if (v & SPI_CMD_USR) {
            bramble_gpspi2_transfer(s);
        }
        break;
    case R_SPI_ADDR:         s->addr = v; break;
    case R_SPI_CTRL:         s->ctrl = v; break;
    case R_SPI_CLOCK:        s->clock = v; break;
    case R_SPI_USER:         s->user = v; break;
    case R_SPI_USER1:        s->user1 = v; break;
    case R_SPI_USER2:        s->user2 = v; break;
    case R_SPI_MS_DLEN:      s->ms_dlen = v; break;
    case R_SPI_MISC:         s->misc = v; break;
    case R_SPI_DMA_CONF:     s->dma_conf = v; break;
    case R_SPI_DMA_INT_ENA:  s->dma_int_ena = v; bramble_gpspi2_update_irq(s); break;
    case R_SPI_DMA_INT_CLR:  s->dma_int_raw &= ~v; bramble_gpspi2_update_irq(s); break;
    case R_SPI_DMA_INT_SET:  s->dma_int_raw |= v; bramble_gpspi2_update_irq(s); break;
    case R_SPI_SLAVE:        s->slave = v; break;
    case R_SPI_SLAVE1:       s->slave1 = v; break;
    case R_SPI_CLK_GATE:     s->clk_gate = v; break;
    case R_SPI_W0 ... R_SPI_W15:
        s->data_reg[(addr - R_SPI_W0) / sizeof(uint32_t)] = v;
        break;
    default:                 break;
    }
}

static const MemoryRegionOps bramble_gpspi2_ops = {
    .read = bramble_gpspi2_read,
    .write = bramble_gpspi2_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void bramble_gpspi2_instance_init(Object *obj)
{
    BrambleGpspi2State *s = BRAMBLE_GPSPI2(obj);

    memory_region_init_io(&s->iomem, obj, &bramble_gpspi2_ops, s,
                          TYPE_BRAMBLE_GPSPI2, 0x1000);

    s->spi = ssi_create_bus(DEVICE(s), "spi");
}

static const TypeInfo bramble_gpspi2_info = {
    .name = TYPE_BRAMBLE_GPSPI2,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(BrambleGpspi2State),
    .instance_init = bramble_gpspi2_instance_init,
};

static void bramble_gpspi2_register_types(void)
{
    type_register_static(&bramble_ledc_info);
    type_register_static(&bramble_ssd1680_info);
    type_register_static(&bramble_sx1262_info);
    type_register_static(&bramble_gpspi2_info);
}

type_init(bramble_gpspi2_register_types)

void bramble_gpspi2_attach(MemoryRegion *sys_mem, DeviceState *gdma,
                           DeviceState *intc)
{
    Object *obj = object_new(TYPE_BRAMBLE_GPSPI2);
    BrambleGpspi2State *s = BRAMBLE_GPSPI2(obj);

    /* Overlay the GPSPI2 window at higher priority than the machine's catch-all
     * IO region (added at priority 0), like bramble_gpio does for GPIO. */
    bramble_overlay_attach(obj, "bramble-gpspi2", &s->iomem, sys_mem,
                           DR_REG_SPI2_BASE,
                           "bramble-gpspi2: controller + SX1262 radio + "
                           "SSD1680 display");

    if (gdma) {
        s->gdma = ESP_GDMA(gdma);
    }
    if (intc) {
        s->intr = qdev_get_gpio_in(intc, ETS_SPI2_INTR_SOURCE);
    }

    /* Attach both bus slaves and capture their CS inputs for routing. Both are
     * register-accurate: the radio is the SX1262 (P2.4a) and the display is the
     * SSD1680 (P2.5). bramble_gpspi2_route drives these SSI_GPIO_CS lines so
     * exactly one answers each transfer. */
    /* Distinct SSI cs_index values so ssi_peripheral_realize's uniqueness check
     * passes; actual selection is driven through each slave's SSI_GPIO_CS input
     * by bramble_gpspi2_route, not this index. */
    DeviceState *radio = qdev_new(TYPE_BRAMBLE_SX1262);
    qdev_prop_set_uint8(radio, "cs", 0);
    ssi_realize_and_unref(radio, s->spi, &error_fatal);
    s->radio_cs = qdev_get_gpio_in_named(radio, SSI_GPIO_CS, 0);

    DeviceState *disp = qdev_new(TYPE_BRAMBLE_SSD1680);
    qdev_prop_set_uint8(disp, "cs", 1);
    ssi_realize_and_unref(disp, s->spi, &error_fatal);
    s->disp_cs = qdev_get_gpio_in_named(disp, SSI_GPIO_CS, 0);

    /* Both slaves idle deselected (CS high). */
    qemu_set_irq(s->radio_cs, 1);
    qemu_set_irq(s->disp_cs, 1);
}
