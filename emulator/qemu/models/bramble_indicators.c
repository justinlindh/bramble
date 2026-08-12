/*
 * Bramble indicator bridge + minimal LEDC model (QEMU esp32s3, Phase 2 emulator).
 *
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
 * imprecise and unmodeled in QEMU), so on/off is the meaningful, reliable
 * signal for "sound is functioning" and the tone is pinned to the known
 * firmware constant. See the LEDC model note below.
 *
 * Threading: note_gpio runs from the GPIO register-write path and note_buzzer
 * from the LEDC register-write path, both on a vCPU thread under the BQL, so
 * the shared snapshot needs no extra lock (same discipline as emulink_send_tx).
 */

#include "qemu/osdep.h"
#include "hw/qdev-core.h"
#include "qom/object.h"
#include "hw/xtensa/bramble_indicators.h"
#include "hw/xtensa/bramble_scaffold.h"
#include "hw/xtensa/bramble_gpio.h"
#include "hw/xtensa/bramble_emulink.h"
#include "hw/misc/esp32s3_reg.h"

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

    uint32_t ch1_conf0;    /* LSCH1_CONF0 latched (SIG_OUT_EN matters) */
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

static void bramble_indicators_register_types(void)
{
    type_register_static(&bramble_ledc_info);
}

type_init(bramble_indicators_register_types)

void bramble_indicators_attach(MemoryRegion *sys_mem)
{
    /* Overlay the LEDC window at higher priority than the machine's catch-all IO
     * region (added at priority 0), like bramble_gpio / bramble_adc. */
    Object *obj = object_new(TYPE_BRAMBLE_LEDC);
    BrambleLedcState *s = BRAMBLE_LEDC(obj);
    bramble_overlay_attach(obj, "bramble-ledc", &s->iomem, sys_mem,
                           DR_REG_LEDC_BASE,
                           "bramble-ledc: buzzer tone overlay");

    /* Forward the pager's LED (GPIO48) and vibra (GPIO16) OUT transitions into
     * the emu-link `ind`; the LEDC overlay above feeds the buzzer side. */
    bramble_gpio_set_out_observer(bramble_ind_note_gpio);
}
