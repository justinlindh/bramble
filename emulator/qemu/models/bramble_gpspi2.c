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
 * CS routing (the seam left for P2.4/P2.5). The pager mixes CS styles:
 *   - Radio (sx1262.c) uses MANUAL software CS: spics_io_num = -1 and the driver
 *     toggles gpio_set_level(GPIO8) by hand; the P2.2 bramble_gpio overlay
 *     already observes GPIO8. So the radio slave's select is derivable from the
 *     GPIO8 level, NOT from this peripheral's CS lines.
 *   - Display (ssd1680.c) uses HARDWARE CS: spics_io_num = GPIO4, driven by this
 *     peripheral through SPI_MISC_REG.CS0_DIS and the GPIO matrix. We drive the
 *     SSI_GPIO_CS out lines (as hw/ssi/esp32s3_spi.c does) around every transfer
 *     so a real hardware-CS slave can be selected in P2.5. For P2.3 a single
 *     stub slave sits on the bus and answers every ssi_transfer regardless of
 *     CS, which is all that is needed to unwedge boot.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-core.h"
#include "qom/object.h"
#include "exec/address-spaces.h"
#include "hw/ssi/ssi.h"
#include "hw/dma/esp_gdma.h"
#include "hw/xtensa/bramble_gpspi2.h"
#include "hw/misc/esp32s3_reg.h"
#include "hw/xtensa/esp32s3_intc.h"

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

/* ---- stub SSI slave ------------------------------------------------------ */

#define TYPE_BRAMBLE_SPI_STUB "bramble.spi-stub"
OBJECT_DECLARE_SIMPLE_TYPE(BrambleSpiStub, BRAMBLE_SPI_STUB)

struct BrambleSpiStub {
    SSIPeripheral parent_obj;
};

/* Drain MOSI, return a benign fixed MISO byte. Correct radio/display responses
 * are P2.4 (SX1262) and P2.5 (SSD1680); here we only need transfers to end. */
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
}

static const TypeInfo bramble_spi_stub_info = {
    .name = TYPE_BRAMBLE_SPI_STUB,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(BrambleSpiStub),
    .class_init = bramble_spi_stub_class_init,
};

/* ---- GPSPI2 controller --------------------------------------------------- */

#define TYPE_BRAMBLE_GPSPI2 "bramble.gpspi2"
OBJECT_DECLARE_SIMPLE_TYPE(BrambleGpspi2State, BRAMBLE_GPSPI2)

struct BrambleGpspi2State {
    DeviceState parent_obj;

    MemoryRegion iomem;
    SSIBus *spi;
    qemu_irq cs_gpio[GPSPI2_CS_COUNT];

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

    /* Latch transfer-done. The pager polls SPI_USR (which we leave clear) and
     * never enables this interrupt, but keep the register state faithful. */
    s->dma_int_raw |= SPI_TRANS_DONE_INT;
    if (s->intr && (s->dma_int_ena & SPI_TRANS_DONE_INT)) {
        qemu_set_irq(s->intr, 1);
        qemu_set_irq(s->intr, 0);
    }
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
    case R_SPI_DMA_INT_ENA:  s->dma_int_ena = v; break;
    case R_SPI_DMA_INT_CLR:  s->dma_int_raw &= ~v; break;
    case R_SPI_DMA_INT_SET:  s->dma_int_raw |= v; break;
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

    /* Attach the stub slave to the SPI2 bus so ssi_transfer always resolves.
     * P2.4/P2.5 replace it with register-accurate SX1262 / SSD1680 slaves. */
    DeviceState *stub = qdev_new(TYPE_BRAMBLE_SPI_STUB);
    ssi_realize_and_unref(stub, s->spi, &error_fatal);

    /* Overlay the GPSPI2 window at higher priority than the machine's catch-all
     * IO region (added at priority 0), like bramble_gpio does for GPIO. */
    memory_region_add_subregion_overlap(sys_mem, DR_REG_SPI2_BASE, &s->iomem, 1);

    fprintf(stderr, "bramble-gpspi2: controller + stub slave attached at 0x%x\n",
            (unsigned)DR_REG_SPI2_BASE);
}
