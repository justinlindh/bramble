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
 * loop. It exposes an SSI bus with two register-accurate slaves attached: the
 * SX1262 radio (bramble_sx1262.c) and the SSD1680 e-paper (bramble_ssd1680.c).
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
 *     register-accurate SX1262 slave (TYPE_BRAMBLE_SX1262, bramble_sx1262.c).
 *   - Display (ssd1680_io.c) uses HARDWARE CS: spics_io_num = GPIO4. The
 *     display is added first (display_init runs before radio_init), so it is
 *     SPI device 0 and owns CS0; the IDF spi_master enables it per transaction
 *     via SPI_MISC_REG.CS0_DIS. P2.5 selects the display POSITIVELY off that
 *     bit (CS0_DIS clear) rather than the P2.4 stub's "GPIO8 not low"
 *     simplification: disp_sel = (GPIO8 high) AND (CS0 enabled). The SSD1680
 *     slave (register-accurate, TYPE_BRAMBLE_SSD1680, bramble_ssd1680.c).
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
#include "hw/ssi/ssi.h"
#include "hw/dma/esp_gdma.h"
#include "hw/xtensa/bramble_gpspi2.h"
#include "hw/xtensa/bramble_gpio.h"
#include "hw/xtensa/bramble_scaffold.h"
#include "hw/xtensa/bramble_sx1262.h"
#include "hw/xtensa/bramble_ssd1680.h"
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

    /* Route to the SX1262 (GPIO8 low) or the display before any byte is
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
     * register-accurate: the radio is the SX1262 (bramble_sx1262.c) and the
     * display is the SSD1680 (bramble_ssd1680.c). bramble_gpspi2_route drives
     * these SSI_GPIO_CS lines so exactly one answers each transfer. */
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
