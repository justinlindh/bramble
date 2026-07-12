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
 * CS routing (implemented in P2.4a, extended by P2.5). The pager mixes CS
 * styles, and two slaves now share the bus (SX1262 radio + display stub), so
 * every transfer must go to exactly one:
 *   - Radio (sx1262.c) uses MANUAL software CS: spics_io_num = -1 and the driver
 *     toggles gpio_set_level(GPIO8) by hand; the P2.2 bramble_gpio overlay
 *     observes GPIO8. So the radio slave's select is derived from the GPIO8
 *     level read back through bramble_gpio_out_level(), NOT from this
 *     peripheral's CS lines. When GPIO8 is low the transfer routes to the
 *     register-accurate SX1262 slave (TYPE_BRAMBLE_SX1262, defined below).
 *   - Display (ssd1680.c) uses HARDWARE CS: spics_io_num = GPIO4, driven by this
 *     peripheral through SPI_MISC_REG.CS0_DIS and the GPIO matrix. We still
 *     drive the SSI_GPIO_CS out lines (as hw/ssi/esp32s3_spi.c does) for a
 *     future real hardware-CS slave, but for now, whenever GPIO8 is high (radio
 *     deselected) the transfer routes to the stub display slave. The SX1262
 *     slave (register-accurate, TYPE_BRAMBLE_SX1262) is defined below.
 * Both slaves are SSI_CS_LOW; bramble_gpspi2_route drives their SSI_GPIO_CS
 * inputs from the GPIO8 decision so exactly one answers each ssi_transfer.
 * P2.5 replaces the stub with a real SSD1680 selected off the hardware CS0
 * line; the radio routing here is unchanged by that.
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
#include "hw/misc/esp32s3_reg.h"
#include "hw/xtensa/esp32s3_intc.h"
#include "chardev/char.h"
#include "chardev/char-fe.h"
#include "qapi/qmp/qjson.h"
#include "qapi/qmp/qdict.h"
#include "qapi/qmp/qobject.h"

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

/* SPI_MISC_REG chip-select disables (active-low CS lines). */
#define SPI_MISC_CS0_DIS   (1u << 0)
#define SPI_MISC_CS1_DIS   (1u << 1)

/* SPI_DMA_CONF_REG data-path enables. */
#define SPI_DMA_TX_ENA     (1u << 28)
#define SPI_DMA_RX_ENA     (1u << 27)

/* SPI_DMA_INT_* transfer-done bit. */
#define SPI_TRANS_DONE_INT (1u << 12)

/* CPU data buffer: 16 words = 64 bytes (SPI_W0..W15). */
#define GPSPI2_BUF_WORDS   16
#define GPSPI2_BUF_BYTES   (GPSPI2_BUF_WORDS * 4)

/* Chip-select out lines exposed to attached slaves (CS0..CS1 used on S3). */
#define GPSPI2_CS_COUNT    2

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

/* base64 encode (RFC 4648, padded); decode lives with the SX1262 slave. */
static const char EMULINK_B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void emulink_b64_encode(const uint8_t *in, size_t n, char *out,
                               size_t out_sz)
{
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        if (o + 4 >= out_sz) {
            break;
        }
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < n) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        if (i + 2 < n) {
            v |= (uint32_t)in[i + 2];
        }
        out[o++] = EMULINK_B64[(v >> 18) & 0x3F];
        out[o++] = EMULINK_B64[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < n) ? EMULINK_B64[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < n) ? EMULINK_B64[v & 0x3F] : '=';
    }
    out[o] = '\0';
}

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
    char b64[352]; /* 4*ceil(255/3)+1 = 341, rounded up */
    emulink_b64_encode(payload, len, b64, sizeof(b64));
    g_autofree char *line = g_strdup_printf(
        "{\"t\":\"tx\",\"payload\":\"%s\",\"freq\":%d,\"sf\":%d,\"bw\":%d,"
        "\"cr\":%d,\"power\":%d}\n",
        b64, freq_mhz, sf, bw_hz, cr, power);
    return emulink_write(line, strlen(line));
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
    (void)qemu_chr_fe_write_all(&s_emulink_chr, (const uint8_t *)hello,
                                strlen(hello));
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

void bramble_emulink_attach(void)
{
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

/* ---- stub SSI slave ------------------------------------------------------ */

#define TYPE_BRAMBLE_SPI_STUB "bramble.spi-stub"
OBJECT_DECLARE_SIMPLE_TYPE(BrambleSpiStub, BRAMBLE_SPI_STUB)

struct BrambleSpiStub {
    SSIPeripheral parent_obj;
};

/* Drain MOSI, return a benign fixed MISO byte. This is now the DISPLAY path
 * only: the radio has a register-accurate SX1262 slave (below) and is routed to
 * it by CS. A correct SSD1680 slave is P2.5; here we only need the e-paper's
 * transfers to end. */
static uint32_t bramble_spi_stub_transfer(SSIPeripheral *dev, uint32_t val)
{
    (void)dev;
    (void)val;
    return 0x00;
}

/* ssi_peripheral_realize() calls ssc->realize unconditionally (no NULL guard),
 * so even a behaviourless stub must provide one. */
static void bramble_spi_stub_realize(SSIPeripheral *dev, Error **errp)
{
    (void)dev;
    (void)errp;
}

static void bramble_spi_stub_class_init(ObjectClass *klass, void *data)
{
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    k->transfer = bramble_spi_stub_transfer;
    k->realize = bramble_spi_stub_realize;
    /* CS-gated so it only answers when selected: with the SX1262 now sharing
     * the bus, an always-on (SSI_CS_NONE) stub would corrupt radio reads by
     * OR-ing 0x00 into every byte. bramble_gpspi2 drives this slave's CS from
     * the routing decision (radio deselected -> display selected). */
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo bramble_spi_stub_info = {
    .name = TYPE_BRAMBLE_SPI_STUB,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(BrambleSpiStub),
    .class_init = bramble_spi_stub_class_init,
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

/* base64 decode (RFC 4648) for inbound `rx` payloads; hand-rolled like
 * radio_virt.c's, no external dep. Returns decoded byte count (<= out_sz). */
static int sx1262_b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static size_t sx1262_b64_decode(const char *in, uint8_t *out, size_t out_sz)
{
    size_t o = 0;
    int quad[4];
    int qi = 0;
    for (const char *p = in; *p; p++) {
        if (*p == '=') {
            break;
        }
        int v = sx1262_b64_val(*p);
        if (v < 0) {
            continue;
        }
        quad[qi++] = v;
        if (qi == 4) {
            if (o + 3 > out_sz) {
                return o;
            }
            out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
            out[o++] = (uint8_t)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
            out[o++] = (uint8_t)(((quad[2] & 0x3) << 6) | quad[3]);
            qi = 0;
        }
    }
    if (qi >= 2 && o < out_sz) {
        out[o++] = (uint8_t)((quad[0] << 2) | (quad[1] >> 4));
    }
    if (qi >= 3 && o < out_sz) {
        out[o++] = (uint8_t)(((quad[1] & 0xF) << 4) | (quad[2] >> 2));
    }
    return o;
}

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
    uint8_t tmp[255];
    size_t n = sx1262_b64_decode(payload, tmp, sizeof(tmp));
    if (n == 0 || n > sizeof(tmp)) {
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
    memcpy(f->data, tmp, n);
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
    qemu_irq cs_gpio[GPSPI2_CS_COUNT];

    /* SSI_GPIO_CS inputs of the two bus slaves, driven per-transfer from the
     * CS-routing decision (bramble_gpspi2_route). radio_cs -> SX1262 slave,
     * disp_cs -> display stub. */
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

/* Select the bus slave this transfer targets. The radio's manual chip select
 * is GPIO8 (driven by sx1262.c via gpio_set_level and observed by the P2.2
 * overlay): GPIO8 low selects the SX1262 slave, otherwise the display stub is
 * selected. Both slaves are SSI_CS_LOW, so a 0 on their SSI_GPIO_CS input means
 * selected. `active` gates whether either is selected (deasserted between
 * transfers so the next assert re-triggers the SX1262's set_cs byte-cursor
 * reset). */
static void bramble_gpspi2_route(BrambleGpspi2State *s, int active)
{
    if (!active) {
        qemu_set_irq(s->radio_cs, 1);
        qemu_set_irq(s->disp_cs, 1);
        return;
    }
    bool radio_sel = (bramble_gpio_out_level(8) == 0);
    qemu_set_irq(s->radio_cs, radio_sel ? 0 : 1);
    qemu_set_irq(s->disp_cs, radio_sel ? 1 : 0);
}

/* Assert (level 0) or deassert (level 1) the enabled CS lines, mirroring
 * hw/ssi/esp32s3_spi.c. A disabled CS (SPI_MISC_REG.CSn_DIS) stays high. */
static void bramble_gpspi2_cs_set(BrambleGpspi2State *s, int active)
{
    int cs0_dis = s->misc & SPI_MISC_CS0_DIS;
    int cs1_dis = s->misc & SPI_MISC_CS1_DIS;
    qemu_set_irq(s->cs_gpio[0], (!cs0_dis && active) ? 0 : 1);
    qemu_set_irq(s->cs_gpio[1], (!cs1_dis && active) ? 0 : 1);
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
    bramble_gpspi2_cs_set(s, 1);

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
        g_autofree uint8_t *txbuf = g_malloc0(data_bytes);
        g_autofree uint8_t *rxbuf = g_malloc0(data_bytes);

        if (do_mosi) {
            if (dma_tx && s->gdma) {
                uint32_t chan;
                if (!esp_gdma_get_channel_periph(s->gdma, GDMA_SPI2,
                                                 ESP_GDMA_OUT_IDX, &chan) ||
                    !esp_gdma_read_channel(s->gdma, chan, txbuf, data_bytes)) {
                    /* Best-effort: a stub slave discards MOSI, so a missed DMA
                     * fetch does not wedge boot. Correct framebuffer bytes are
                     * a P2.5 concern (see channel-disambiguation note below). */
                    qemu_log_mask(LOG_UNIMP,
                        "bramble-gpspi2: SPI2 GDMA OUT fetch failed (%u bytes)\n",
                        data_bytes);
                }
            } else {
                memcpy(txbuf, s->data_reg, MIN(data_bytes, GPSPI2_BUF_BYTES));
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

    bramble_gpspi2_cs_set(s, 0);
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
    qdev_init_gpio_out_named(DEVICE(s), &s->cs_gpio[0], SSI_GPIO_CS,
                             GPSPI2_CS_COUNT);
}

static const TypeInfo bramble_gpspi2_info = {
    .name = TYPE_BRAMBLE_GPSPI2,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(BrambleGpspi2State),
    .instance_init = bramble_gpspi2_instance_init,
};

static void bramble_gpspi2_register_types(void)
{
    type_register_static(&bramble_spi_stub_info);
    type_register_static(&bramble_sx1262_info);
    type_register_static(&bramble_gpspi2_info);
}

type_init(bramble_gpspi2_register_types)

void bramble_gpspi2_attach(MemoryRegion *sys_mem, DeviceState *gdma,
                           DeviceState *intc)
{
    Object *obj = object_new(TYPE_BRAMBLE_GPSPI2);
    object_property_add_child(qdev_get_machine(), "bramble-gpspi2", obj);
    qdev_realize(DEVICE(obj), NULL, &error_fatal);

    BrambleGpspi2State *s = BRAMBLE_GPSPI2(obj);
    if (gdma) {
        s->gdma = ESP_GDMA(gdma);
    }
    if (intc) {
        s->intr = qdev_get_gpio_in(intc, ETS_SPI2_INTR_SOURCE);
    }

    /* Attach both bus slaves and capture their CS inputs for routing. The
     * radio is the register-accurate SX1262 (P2.4a); the display is still the
     * stub (P2.5 replaces it with a real SSD1680). bramble_gpspi2_route drives
     * these SSI_GPIO_CS lines so exactly one answers each transfer. */
    /* Distinct SSI cs_index values so ssi_peripheral_realize's uniqueness check
     * passes; actual selection is driven through each slave's SSI_GPIO_CS input
     * by bramble_gpspi2_route, not this index. */
    DeviceState *radio = qdev_new(TYPE_BRAMBLE_SX1262);
    qdev_prop_set_uint8(radio, "cs", 0);
    ssi_realize_and_unref(radio, s->spi, &error_fatal);
    s->radio_cs = qdev_get_gpio_in_named(radio, SSI_GPIO_CS, 0);

    DeviceState *disp = qdev_new(TYPE_BRAMBLE_SPI_STUB);
    qdev_prop_set_uint8(disp, "cs", 1);
    ssi_realize_and_unref(disp, s->spi, &error_fatal);
    s->disp_cs = qdev_get_gpio_in_named(disp, SSI_GPIO_CS, 0);

    /* Both slaves idle deselected (CS high). */
    qemu_set_irq(s->radio_cs, 1);
    qemu_set_irq(s->disp_cs, 1);

    /* Overlay the GPSPI2 window at higher priority than the machine's catch-all
     * IO region (added at priority 0), like bramble_gpio does for GPIO. */
    memory_region_add_subregion_overlap(sys_mem, DR_REG_SPI2_BASE, &s->iomem, 1);

    fprintf(stderr, "bramble-gpspi2: controller + SX1262 radio + display stub "
            "attached at 0x%x\n", (unsigned)DR_REG_SPI2_BASE);
}
