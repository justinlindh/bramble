/*
 * Battery backend for the SenseCAP T1000-E: charge detection, no cell
 * voltage. Satisfies components/battery/include/battery.h.
 *
 * This board reports whether a charger is driving the rail and reports no
 * voltage reading at all: mv 0, pct 0, present false. That is deliberate,
 * and it is the honest answer rather than a placeholder.
 *
 * P0.02/AIN0 does reach the cell through a 2x divider, but the divider
 * hangs off a gated sensor rail. With the rail off the SAADC conversions
 * succeed and return 1 to 4 mV, which is not a failed read a driver can
 * detect and discard: it is a confident wrong answer. A voltage backend
 * built on it reports roughly 0 percent whenever the device is off the
 * charger, trips the display's low-battery floor, and beacons a dying cell
 * to the mesh.
 *
 * Energizing the gate is not an available workaround. The vendor SDK
 * (Seeed-Tracker-T1000-E-for-LoRaWAN-dev-board, smtc_hal_config.h) names
 * P1.06 SENSE_POWER_EN as the rail enable, and driving it high on hardware
 * stopped the board within about a millisecond: the flash boot trace ends
 * on the stamp written immediately after the pin write, with no failure
 * tag from any of the instrumented fatal paths and no rescue from the
 * no-advertising sentinel, so the board reset or locked up rather than
 * blocking. Which of those it was is not established. Until it is, this
 * port does not touch that pin, and no build here configures it.
 *
 * Charge detection needs neither the ADC nor that rail: both detect pins
 * are plain GPIO inputs. Their verdicts are confirmed on hardware, tracking
 * a charger being plugged and unplugged exactly.
 *
 * Detect-pin wiring verified 2026-08-01 against Meshtastic's
 * github.com/meshtastic/firmware, variants/nrf52840/tracker-t1000-e/
 * variant.h: EXT_CHRG_DETECT (32+3)/P1.03, EXT_PWR_DETECT (0+5)/P0.05. The
 * pin MODES (pull-up vs none) are not in variant.h; src/Power.cpp is the
 * consumer and its defaults apply here because neither this variant nor any
 * shared header overrides them: EXT_CHRG_DETECT_MODE and EXT_PWR_DETECT_MODE
 * both default to plain INPUT (no internal pull), and EXT_CHRG_DETECT_VALUE
 * is LOW (charging), EXT_PWR_DETECT_VALUE is HIGH (external power),
 * confirmed by a repo-wide search turning up no override anywhere.
 * isCharging() for a board with EXT_CHRG_DETECT defined (this one) is
 * `digitalRead(EXT_CHRG_DETECT) == EXT_CHRG_DETECT_VALUE`, i.e. charging =
 * P1.03 low.
 */
#include "battery.h"

#include <string.h>

#include <hal/nrf_gpio.h>

#include "bramble_board.h"
#include "esp_log.h"

static const char* TAG = "battery_t1000e";

void battery_init(void) {
    /* CHRG/VBUS detect: plain inputs, no pull (see file header for the
     * Power.cpp citation backing this over a pull-up guess). Nothing else
     * to bring up: there is no ADC channel and no rail to gate. */
    nrf_gpio_cfg_input(BOARD_PIN_CHRG_DETECT, NRF_GPIO_PIN_NOPULL);
    nrf_gpio_cfg_input(BOARD_PIN_VBUS_DETECT, NRF_GPIO_PIN_NOPULL);
    ESP_LOGI(TAG, "charge detect ready (CHRG P1.%02d, VBUS P0.%02d); no cell voltage on this board",
             BOARD_PIN_CHRG_DETECT - 32, BOARD_PIN_VBUS_DETECT);
}

void battery_get_status(battery_status_t* out) {
    /* mv, pct and present all stay zero: present false is what makes
     * battery_reading_available() false, which is what makes beacons send
     * the unknown sentinel and displays show "no reading" instead of a
     * number. See the file header for why there is no reading to give. */
    memset(out, 0, sizeof(*out));

    int chrg_level = nrf_gpio_pin_read(BOARD_PIN_CHRG_DETECT);
    battery_charging_t chrg =
        battery_charging_from_gpio(BOARD_PIN_CHRG_DETECT, 0 /* active LOW */, chrg_level);

    /* VBUS folds into the single "charging" signal rather than staying
     * separate: battery_status_t has no distinct "externally powered but
     * not charging" state. This is not a gap Meshtastic's firmware avoids
     * either: its isVbusIn() (src/Power.cpp) is exactly this same OR,
     * VBUS-high or CHRG-active, for precisely the reason this code needs
     * it, EXT_PWR_DETECT alone misses a charge-complete pin that de-asserts
     * CHRG while a charger is still connected. Meshtastic's CHRG-only
     * signal is the separate isCharging(); our single `charging` field
     * plays the role of its isVbusIn(), "a charger is driving the rail
     * right now", not strictly "current is flowing into the cell". */
    bool vbus_present = nrf_gpio_pin_read(BOARD_PIN_VBUS_DETECT) != 0;
    out->charging = (chrg == BATTERY_CHG_YES || vbus_present) ? BATTERY_CHG_YES : BATTERY_CHG_NO;
}
