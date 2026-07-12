/*
 * Bramble SAR ADC oneshot stub (QEMU esp32s3, Phase 2 milestone P2.3b).
 *
 * P2.3 unwedged the GPSPI2 spin so the pager firmware now boots through
 * show_splash / button_init / battery_init and then WEDGES at the first SAR ADC
 * oneshot conversion in battery_read_mv() (components/battery/battery.c). The
 * IDF oneshot driver kicks a conversion and then spins in
 * adc_oneshot_ll_get_event() waiting on a "done" bit that never sets, because
 * the SAR ADC is not modeled. (P2.1 stubbed ADC *calibration* via the eFuse
 * image; that gets past self-calibration, but the first real *conversion* now
 * spins.)
 *
 * On the esp32s3 the ADC oneshot ("RTC single read") path drives the SENS
 * peripheral, NOT the APB_SARADC digital controller (hal/esp32s3/adc_ll.h):
 *   - adc_oneshot_ll_start(): waits for SENS.sar_slave_addr1.meas_status == 0,
 *     then pulses SENS.sar_meas1_ctrl2.meas1_start_sar 0 -> 1.
 *   - adc_oneshot_ll_get_event(): polls SENS.sar_meas1_ctrl2.meas1_done_sar.
 *   - adc_oneshot_ll_get_raw_result(): reads SENS.sar_meas1_ctrl2.meas1_data_sar.
 * ADC_UNIT_2 uses the parallel sar_meas2_ctrl2 register with the same layout.
 * The stock espressif/qemu esp32s3 machine models RTC_CNTL (DR_REG_RTCCNTL_BASE)
 * but leaves the SENS window (DR_REG_SENS_BASE, 0x60008800) unmapped, so those
 * reads hit the catch-all IO region and meas_done_sar reads 0 forever.
 *
 * Like bramble_gpio / bramble_gpspi2, this model installs a higher-priority
 * MemoryRegion overlay over the SENS window and decodes just the three
 * registers the oneshot poll touches: when the driver writes the START bit we
 * immediately latch DONE and a fixed, plausible raw result; the idle-status
 * register reads back 0 so the pre-conversion wait loop falls through. Every
 * other SENS register reads 0 / drops writes exactly as the catch-all did, so
 * the ADC-config path the firmware already completed is unchanged. Battery
 * voltage is not load-bearing for the emulator; any finite result that lets the
 * poll finish is enough. BRAMBLE_ADC_RAW is a mid-scale 12-bit reading (~1.6V
 * at the pin) which maps, through battery.c's calibration and divider, to a
 * sane multi-volt battery level.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/qdev-core.h"
#include "qom/object.h"
#include "exec/address-spaces.h"
#include "hw/xtensa/bramble_adc.h"
#include "hw/xtensa/bramble_scaffold.h"
#include "hw/misc/esp32s3_reg.h"

/* SENS register offsets, relative to DR_REG_SENS_BASE (soc/sens_reg.h). The
 * SENS window runs to DR_REG_RTC_I2C_BASE (+0x400); overlaying 0x400 covers
 * every SENS register without shadowing the neighbouring RTC peripherals. */
#define R_SAR_MEAS1_CTRL2   0x00C  /* ADC1 start/done/data */
#define R_SAR_MEAS2_CTRL2   0x030  /* ADC2 start/done/data */
#define R_SAR_SLAVE_ADDR1   0x040  /* holds meas_status (idle) [29:22] */

#define BRAMBLE_SENS_WINDOW 0x400

/* sar_measN_ctrl2 bit layout (identical for ADC1 and ADC2). */
#define SAR_MEAS_DATA_MASK  0x0000FFFFu /* [15:0]  RO converted value */
#define SAR_MEAS_DONE       (1u << 16)  /* [16]    RO conversion-done flag */
#define SAR_MEAS_START      (1u << 17)  /* [17]    R/W software start pulse */

/* Fixed raw conversion result handed back for every oneshot read. Mid-scale of
 * the 12-bit SAR range; the exact value does not matter to the emulator. */
#define BRAMBLE_ADC_RAW     2048u

#define TYPE_BRAMBLE_ADC "bramble.adc"
OBJECT_DECLARE_SIMPLE_TYPE(BrambleAdcState, BRAMBLE_ADC)

struct BrambleAdcState {
    DeviceState parent_obj;

    MemoryRegion iomem;

    /* Latched sar_measN_ctrl2 register values (DONE + data after a start). */
    uint32_t meas1;
    uint32_t meas2;
};

/* Complete a oneshot conversion synchronously: latch DONE and the fixed raw
 * result so the very next done-poll returns "finished". */
static void bramble_adc_convert(BrambleAdcState *s, int unit)
{
    uint32_t reg = SAR_MEAS_DONE | (BRAMBLE_ADC_RAW & SAR_MEAS_DATA_MASK);
    if (unit == 1) {
        s->meas1 = reg;
    } else {
        s->meas2 = reg;
    }
    fprintf(stderr, "bramble-adc: oneshot unit=ADC%d -> raw=%u (done)\n",
            unit, BRAMBLE_ADC_RAW);
}

static uint64_t bramble_adc_read(void *opaque, hwaddr addr, unsigned int size)
{
    BrambleAdcState *s = BRAMBLE_ADC(opaque);
    switch (addr) {
    case R_SAR_MEAS1_CTRL2: return s->meas1;
    case R_SAR_MEAS2_CTRL2: return s->meas2;
    /* meas_status == 0 means the SAR controller is idle; the driver's
     * pre-start wait loop spins until it reads 0, so answer 0. */
    case R_SAR_SLAVE_ADDR1: return 0;
    default:                return 0;
    }
}

static void bramble_adc_write(void *opaque, hwaddr addr, uint64_t value,
                              unsigned int size)
{
    BrambleAdcState *s = BRAMBLE_ADC(opaque);
    uint32_t v = (uint32_t)value;
    switch (addr) {
    case R_SAR_MEAS1_CTRL2:
        /* The driver pulses START 0 -> 1 via read-modify-write of the whole
         * register. Run the conversion on the START-set write and leave DONE
         * latched; other writes (e.g. clearing START to 0) store verbatim. */
        if (v & SAR_MEAS_START) {
            bramble_adc_convert(s, 1);
        } else {
            s->meas1 = v;
        }
        break;
    case R_SAR_MEAS2_CTRL2:
        if (v & SAR_MEAS_START) {
            bramble_adc_convert(s, 2);
        } else {
            s->meas2 = v;
        }
        break;
    default:
        break;
    }
}

static const MemoryRegionOps bramble_adc_ops = {
    .read = bramble_adc_read,
    .write = bramble_adc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void bramble_adc_instance_init(Object *obj)
{
    BrambleAdcState *s = BRAMBLE_ADC(obj);

    memory_region_init_io(&s->iomem, obj, &bramble_adc_ops, s,
                          TYPE_BRAMBLE_ADC, BRAMBLE_SENS_WINDOW);
}

static const TypeInfo bramble_adc_info = {
    .name = TYPE_BRAMBLE_ADC,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(BrambleAdcState),
    .instance_init = bramble_adc_instance_init,
};

static void bramble_adc_register_types(void)
{
    type_register_static(&bramble_adc_info);
}

type_init(bramble_adc_register_types)

void bramble_adc_attach(MemoryRegion *sys_mem)
{
    Object *obj = object_new(TYPE_BRAMBLE_ADC);
    BrambleAdcState *s = BRAMBLE_ADC(obj);

    /* Overlay the SENS window at higher priority than the machine's catch-all
     * IO region (added at priority 0), like bramble_gpio / bramble_gpspi2. */
    bramble_overlay_attach(obj, "bramble-adc", &s->iomem, sys_mem,
                           DR_REG_SENS_BASE,
                           "bramble-adc: SAR ADC oneshot stub");
}
