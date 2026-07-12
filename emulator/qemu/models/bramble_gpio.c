/*
 * Bramble GPIO observer/injector (QEMU esp32s3, Phase 2 milestone P2.2).
 *
 * The espressif/qemu esp32s3 GPIO model (hw/gpio/esp32s3_gpio.c, a thin
 * subclass of hw/gpio/esp32_gpio.c) is a near-stub: it answers the GPIO_STRAP
 * read for boot straps and otherwise no-ops every write and returns 0 for
 * every read. It exposes no per-pin qemu_irq out lines and no qdev_get_gpio_in
 * inputs, so the "connect observer IRQ sinks" design the P2.2 brief sketched is
 * not available: there is nothing to sink from.
 *
 * Rather than rewrite the shared esp32_gpio base register decode (which esp32,
 * esp32c3 and esp32s3 all share), this model installs a higher-priority
 * MemoryRegion overlay over the GPIO peripheral window (DR_REG_GPIO_BASE,
 * 0x1000). The overlay is a behavioural superset of the stub: it replicates the
 * only meaningful stub behaviour (GPIO_STRAP -> strap mode) and additionally
 *   - decodes the OUT / OUT_W1TS / OUT_W1TC (+ OUT1 for pins 32..48) writes so
 *     every output level transition the firmware drives is logged as a
 *     greppable "bramble-gpio: OUT ..." line (event wiring to emu-link is a
 *     LATER milestone; here we only observe);
 *   - serves GPIO_IN / GPIO_IN1 reads from an injectable input bitmap so a
 *     button press asserted from outside the VM changes what gpio_get_level()
 *     sees;
 *   - latches GPIO_STATUS and pulses the interrupt-matrix GPIO source on a
 *     press edge, so the interrupt path a real edge drives is exercised.
 *
 * Because the overlay fully shadows the stub for the whole window and
 * replicates its strap behaviour, boot is unchanged (same GPSPI2 wedge).
 *
 * Button injection transport is PROVISIONAL: three QMP-settable bool
 * properties (select/up/down) on /machine/bramble-gpio. The gosim emu-link
 * bridge (P2.4 shim / P2.6 integration) will own the real transport; this is
 * the minimum surface that proves the injection path end to end today.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/qdev-core.h"
#include "qom/object.h"
#include "exec/address-spaces.h"
#include "hw/xtensa/bramble_gpio.h"
#include "hw/misc/esp32s3_reg.h"
#include "hw/xtensa/esp32s3_intc.h"

/* GPIO peripheral register offsets (soc/gpio_reg.h, esp32s3). Banked: the
 * plain registers cover pins 0..31, the "1" variants cover pins 32..48. */
#define R_GPIO_OUT          0x0004
#define R_GPIO_OUT_W1TS     0x0008
#define R_GPIO_OUT_W1TC     0x000C
#define R_GPIO_OUT1         0x0010
#define R_GPIO_OUT1_W1TS    0x0014
#define R_GPIO_OUT1_W1TC    0x0018
#define R_GPIO_ENABLE       0x0020
#define R_GPIO_ENABLE_W1TS  0x0024
#define R_GPIO_ENABLE_W1TC  0x0028
#define R_GPIO_ENABLE1      0x002C
#define R_GPIO_ENABLE1_W1TS 0x0030
#define R_GPIO_ENABLE1_W1TC 0x0034
#define R_GPIO_STRAP        0x0038
#define R_GPIO_IN           0x003C
#define R_GPIO_IN1          0x0040
#define R_GPIO_STATUS       0x0044
#define R_GPIO_STATUS_W1TS  0x0048
#define R_GPIO_STATUS_W1TC  0x004C
#define R_GPIO_STATUS1      0x0050
#define R_GPIO_STATUS1_W1TS 0x0054
#define R_GPIO_STATUS1_W1TC 0x0058

/* esp32s3 flash-boot strap value, matching esp32s3_gpio.c's default. */
#define BRAMBLE_STRAP_MODE_FLASH_BOOT 0x4

#define TYPE_BRAMBLE_GPIO "bramble.gpio"
OBJECT_DECLARE_SIMPLE_TYPE(BrambleGpioState, BRAMBLE_GPIO)

/* One injectable button: pin, human name, and the QMP property name. */
typedef struct {
    int pin;
    const char *name;
    const char *prop;
} BrambleButton;

/* Pager HMI buttons (main/boards/bramble_pager.h): active LOW with internal
 * pull-ups, so released == 1, pressed == 0. SELECT is the BOOT/strap pin 0. */
static const BrambleButton bramble_buttons[] = {
    { 0,  "SELECT", "select" },
    { 21, "UP",     "up"     },
    { 47, "DOWN",   "down"   },
};

/* Friendly names for the output pins the pager firmware drives, so the log is
 * readable. Alert outputs (LED/vibra/GNSS_EN) only fire after the main loop
 * starts, which is past the P2.3 GPSPI2 wedge; the SPI CS / reset / DC pins
 * fire in board_init, before the wedge, and are what P2.2 can observe today.
 * buzzer=GPIO15 is intentionally absent: the firmware drives it via LEDC PWM
 * (components/indicators/indicators.c), not gpio_set_level, so a plain GPIO
 * model never sees it (LEDC is unmodeled on S3; deferred to a later touch). */
typedef struct {
    int pin;
    const char *name;
} BramblePinName;

static const BramblePinName bramble_out_names[] = {
    { 48, "LED" },       /* alert output, post-wedge */
    { 16, "VIBRA" },     /* alert output, post-wedge */
    { 38, "GNSS_EN" },   /* GNSS power gate, active low, post-wedge */
    { 8,  "RADIO_CS" },  /* board_init, pre-wedge */
    { 12, "RADIO_RST" }, /* board_init, pre-wedge */
    { 4,  "EPD_CS" },    /* board_init, pre-wedge */
    { 5,  "EPD_DC" },    /* board_init, pre-wedge */
    { 6,  "EPD_RST" },   /* board_init, pre-wedge */
    { 9,  "SPI_SCK" },   /* board_init, pre-wedge */
    { 10, "SPI_MOSI" },  /* board_init, pre-wedge */
};

struct BrambleGpioState {
    DeviceState parent_obj;

    MemoryRegion iomem;

    /* Bank 0 = pins 0..31, bank 1 = pins 32..48. */
    uint32_t out[2];      /* driven output level, as the OUT registers hold */
    uint32_t enable[2];   /* output-enable, tracked for completeness */
    uint32_t in[2];       /* input level served to GPIO_IN reads */
    uint32_t status[2];   /* latched interrupt status */

    /* Interrupt-matrix input for ETS_GPIO_INTR_SOURCE (a real edge would
     * raise this). NULL if the machine did not wire it. */
    qemu_irq intr;
};

/* Singleton, set at attach: lets sibling models (bramble_gpspi2's CS routing)
 * read a driven output level without duplicating GPIO state. */
static BrambleGpioState *s_bramble_gpio;

/* Current driven level of an output pin (0..48), as the OUT registers hold it.
 * Returns 0 if the overlay is not attached. Used by bramble_gpspi2 to route
 * SPI transfers by the radio's manual CS on GPIO8. */
bool bramble_gpio_out_level(int pin)
{
    if (!s_bramble_gpio || pin < 0 || pin > 48) {
        return false;
    }
    return (s_bramble_gpio->out[pin / 32] >> (pin % 32)) & 1;
}

static const char *bramble_out_name(int pin)
{
    for (size_t i = 0; i < ARRAY_SIZE(bramble_out_names); i++) {
        if (bramble_out_names[i].pin == pin) {
            return bramble_out_names[i].name;
        }
    }
    return "gpio";
}

/* Apply a new OUT-bank value, logging every level transition. */
static void bramble_gpio_set_out(BrambleGpioState *s, int bank, uint32_t val)
{
    uint32_t changed = s->out[bank] ^ val;
    s->out[bank] = val;
    while (changed) {
        int bit = ctz32(changed);
        changed &= changed - 1;
        int pin = bank * 32 + bit;
        int level = (val >> bit) & 1;
        fprintf(stderr, "bramble-gpio: OUT gpio=%d(%s) level=%d\n",
                pin, bramble_out_name(pin), level);
    }
}

static uint64_t bramble_gpio_read(void *opaque, hwaddr addr, unsigned int size)
{
    BrambleGpioState *s = BRAMBLE_GPIO(opaque);
    switch (addr) {
    case R_GPIO_OUT:      return s->out[0];
    case R_GPIO_OUT1:     return s->out[1];
    case R_GPIO_ENABLE:   return s->enable[0];
    case R_GPIO_ENABLE1:  return s->enable[1];
    case R_GPIO_STRAP:    return BRAMBLE_STRAP_MODE_FLASH_BOOT;
    case R_GPIO_IN:       return s->in[0];
    case R_GPIO_IN1:      return s->in[1];
    case R_GPIO_STATUS:   return s->status[0];
    case R_GPIO_STATUS1:  return s->status[1];
    default:              return 0;
    }
}

static void bramble_gpio_write(void *opaque, hwaddr addr, uint64_t value,
                               unsigned int size)
{
    BrambleGpioState *s = BRAMBLE_GPIO(opaque);
    uint32_t v = (uint32_t)value;
    switch (addr) {
    case R_GPIO_OUT:          bramble_gpio_set_out(s, 0, v); break;
    case R_GPIO_OUT_W1TS:     bramble_gpio_set_out(s, 0, s->out[0] | v); break;
    case R_GPIO_OUT_W1TC:     bramble_gpio_set_out(s, 0, s->out[0] & ~v); break;
    case R_GPIO_OUT1:         bramble_gpio_set_out(s, 1, v); break;
    case R_GPIO_OUT1_W1TS:    bramble_gpio_set_out(s, 1, s->out[1] | v); break;
    case R_GPIO_OUT1_W1TC:    bramble_gpio_set_out(s, 1, s->out[1] & ~v); break;
    case R_GPIO_ENABLE:       s->enable[0] = v; break;
    case R_GPIO_ENABLE_W1TS:  s->enable[0] |= v; break;
    case R_GPIO_ENABLE_W1TC:  s->enable[0] &= ~v; break;
    case R_GPIO_ENABLE1:      s->enable[1] = v; break;
    case R_GPIO_ENABLE1_W1TS: s->enable[1] |= v; break;
    case R_GPIO_ENABLE1_W1TC: s->enable[1] &= ~v; break;
    case R_GPIO_STATUS:       s->status[0] = v; break;
    case R_GPIO_STATUS_W1TS:  s->status[0] |= v; break;
    case R_GPIO_STATUS_W1TC:  s->status[0] &= ~v; break;
    case R_GPIO_STATUS1:      s->status[1] = v; break;
    case R_GPIO_STATUS1_W1TS: s->status[1] |= v; break;
    case R_GPIO_STATUS1_W1TC: s->status[1] &= ~v; break;
    default:                  break;
    }
}

static const MemoryRegionOps bramble_gpio_ops = {
    .read = bramble_gpio_read,
    .write = bramble_gpio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

/* Drive one button's pin to its pressed/released input level and, on a press
 * (falling) edge, latch GPIO_STATUS and pulse the interrupt-matrix source. */
static void bramble_gpio_inject(BrambleGpioState *s, const BrambleButton *b,
                                bool pressed)
{
    int bank = b->pin / 32;
    int bit = b->pin % 32;
    int level = pressed ? 0 : 1; /* active low */
    int old = (s->in[bank] >> bit) & 1;

    if (level) {
        s->in[bank] |= (1u << bit);
    } else {
        s->in[bank] &= ~(1u << bit);
    }
    fprintf(stderr, "bramble-gpio: BTN %s %s IN gpio=%d level=%d\n",
            b->name, pressed ? "press" : "release", b->pin, level);

    if (old == 1 && level == 0) {
        /* Falling edge: what a real button press raises. */
        s->status[bank] |= (1u << bit);
        if (s->intr) {
            qemu_set_irq(s->intr, 1);
            qemu_set_irq(s->intr, 0);
        }
        fprintf(stderr,
                "bramble-gpio: INTR raised gpio=%d (ETS_GPIO_INTR_SOURCE)\n",
                b->pin);
    }
}

/* Each button gets its own bool property; index encodes which one. */
static const BrambleButton *bramble_button_for_prop(const char *prop)
{
    for (size_t i = 0; i < ARRAY_SIZE(bramble_buttons); i++) {
        if (strcmp(bramble_buttons[i].prop, prop) == 0) {
            return &bramble_buttons[i];
        }
    }
    return NULL;
}

static void bramble_gpio_set_button(Object *obj, bool pressed, Error **errp,
                                    const char *prop)
{
    BrambleGpioState *s = BRAMBLE_GPIO(obj);
    const BrambleButton *b = bramble_button_for_prop(prop);
    if (b) {
        bramble_gpio_inject(s, b, pressed);
    }
}

static bool bramble_gpio_get_button(Object *obj, const char *prop)
{
    BrambleGpioState *s = BRAMBLE_GPIO(obj);
    const BrambleButton *b = bramble_button_for_prop(prop);
    if (!b) {
        return false;
    }
    /* pressed == input level low */
    return ((s->in[b->pin / 32] >> (b->pin % 32)) & 1) == 0;
}

static void bramble_set_select(Object *o, bool v, Error **e) { bramble_gpio_set_button(o, v, e, "select"); }
static void bramble_set_up(Object *o, bool v, Error **e)     { bramble_gpio_set_button(o, v, e, "up"); }
static void bramble_set_down(Object *o, bool v, Error **e)   { bramble_gpio_set_button(o, v, e, "down"); }
static bool bramble_get_select(Object *o, Error **e) { return bramble_gpio_get_button(o, "select"); }
static bool bramble_get_up(Object *o, Error **e)     { return bramble_gpio_get_button(o, "up"); }
static bool bramble_get_down(Object *o, Error **e)   { return bramble_gpio_get_button(o, "down"); }

static void bramble_gpio_instance_init(Object *obj)
{
    BrambleGpioState *s = BRAMBLE_GPIO(obj);

    memory_region_init_io(&s->iomem, obj, &bramble_gpio_ops, s,
                          TYPE_BRAMBLE_GPIO, 0x1000);

    /* Buttons idle released: active-low with pull-ups reads high. */
    for (size_t i = 0; i < ARRAY_SIZE(bramble_buttons); i++) {
        int pin = bramble_buttons[i].pin;
        s->in[pin / 32] |= (1u << (pin % 32));
    }

    object_property_add_bool(obj, "select", bramble_get_select, bramble_set_select);
    object_property_add_bool(obj, "up", bramble_get_up, bramble_set_up);
    object_property_add_bool(obj, "down", bramble_get_down, bramble_set_down);
}

static const TypeInfo bramble_gpio_info = {
    .name = TYPE_BRAMBLE_GPIO,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(BrambleGpioState),
    .instance_init = bramble_gpio_instance_init,
};

static void bramble_gpio_register_types(void)
{
    type_register_static(&bramble_gpio_info);
}

type_init(bramble_gpio_register_types)

void bramble_gpio_attach(MemoryRegion *sys_mem, DeviceState *intc)
{
    Object *obj = object_new(TYPE_BRAMBLE_GPIO);
    /* Give it a canonical path (/machine/bramble-gpio) so QMP qom-set can
     * reach the button-injection properties. */
    object_property_add_child(qdev_get_machine(), "bramble-gpio", obj);
    qdev_realize(DEVICE(obj), NULL, &error_fatal);

    BrambleGpioState *s = BRAMBLE_GPIO(obj);
    s_bramble_gpio = s;
    if (intc) {
        s->intr = qdev_get_gpio_in(intc, ETS_GPIO_INTR_SOURCE);
    }

    /* Overlay the GPIO window at higher priority than the stock esp32s3 GPIO
     * model (added at priority 0), so our decode services every access. */
    memory_region_add_subregion_overlap(sys_mem, DR_REG_GPIO_BASE,
                                         &s->iomem, 1);

    fprintf(stderr, "bramble-gpio: observer/injector attached at 0x%x\n",
            (unsigned)DR_REG_GPIO_BASE);
}
