/*
 * Bramble SX1262 LoRa radio SSI slave (QEMU esp32s3, Phase 2 emulator, P2.4).
 *
 * Register-accurate model of the Semtech SX1262 the pager's radio driver
 * (components/radio/sx1262.c) talks to over SPI2. It answers radio_init's
 * command stream correctly, round-trips register/buffer reads, and layers the
 * emu-link TX/RX + DIO1 IRQ on the state kept here so the QEMU pager meshes with
 * the linux pagers over the gosim ether.
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
 * BUSY (GPIO13) is served low by the P2.2 overlay; DIO1 (RX/TX-done IRQ) is
 * driven here via the GPIO overlay's input accessor on emu-link TxDone/RxDone.
 */

#include "qemu/osdep.h"
#include "hw/qdev-core.h"
#include "qom/object.h"
#include "hw/ssi/ssi.h"
#include "qemu/base64.h"
#include "qapi/qmp/qdict.h"
#include "hw/xtensa/bramble_sx1262.h"
#include "hw/xtensa/bramble_gpio.h"
#include "hw/xtensa/bramble_emulink.h"

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

static void bramble_sx1262_register_types(void)
{
    type_register_static(&bramble_sx1262_info);
}

type_init(bramble_sx1262_register_types)
