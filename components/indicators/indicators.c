/*
 * indicators (on-device): a trivial GPIO/LEDC wrapper for the pager's alert
 * outputs. Pins are pager-specific (LED = GPIO48, buzzer = GPIO15 via an LEDC
 * tone, vibra = GPIO16); the host/emulator half lives in indicator_virt.c.
 *
 * These outputs are driven by the alert path (components/indicators/alerts.c),
 * which main wires up at boot via indicator_init() / alerts_init() and feeds
 * from message-received and periodic tick events. Both platform halves present
 * the same indicators.h so callers route through a single seam.
 */

/* CONFIG_IDF_TARGET_LINUX lives in sdkconfig.h (same convention as
 * board_config.h / display.h); the host half lives in indicator_virt.c. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_LINUX)

#include "indicators.h"

#include "driver/gpio.h"
#include "driver/ledc.h"

#define IND_LED_GPIO 48
#define IND_VIBRA_GPIO 16
#define IND_BUZZER_GPIO 15

#define IND_BUZZER_MODE LEDC_LOW_SPEED_MODE
#define IND_BUZZER_TIMER LEDC_TIMER_1
#define IND_BUZZER_CHANNEL LEDC_CHANNEL_1
#define IND_BUZZER_RES LEDC_TIMER_10_BIT

static bool s_ready = false;

void indicator_init(void) {
    gpio_config_t out = {
        .pin_bit_mask = (1ULL << IND_LED_GPIO) | (1ULL << IND_VIBRA_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);
    gpio_set_level(IND_LED_GPIO, 0);
    gpio_set_level(IND_VIBRA_GPIO, 0);

    ledc_timer_config_t timer = {
        .speed_mode = IND_BUZZER_MODE,
        .timer_num = IND_BUZZER_TIMER,
        .duty_resolution = IND_BUZZER_RES,
        .freq_hz = 2000, /* placeholder; real frequency set per-tone */
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {
        .speed_mode = IND_BUZZER_MODE,
        .channel = IND_BUZZER_CHANNEL,
        .timer_sel = IND_BUZZER_TIMER,
        .intr_type = LEDC_INTR_DISABLE,
        .gpio_num = IND_BUZZER_GPIO,
        .duty = 0, /* silent until indicator_buzzer() */
        .hpoint = 0,
    };
    ledc_channel_config(&channel);
    s_ready = true;
}

void indicator_set_led(bool on) {
    if (!s_ready)
        return;
    gpio_set_level(IND_LED_GPIO, on ? 1 : 0);
}

void indicator_buzzer(uint32_t hz_or_0) {
    if (!s_ready)
        return;
    if (hz_or_0 == 0) {
        ledc_set_duty(IND_BUZZER_MODE, IND_BUZZER_CHANNEL, 0);
        ledc_update_duty(IND_BUZZER_MODE, IND_BUZZER_CHANNEL);
        return;
    }
    ledc_set_freq(IND_BUZZER_MODE, IND_BUZZER_TIMER, hz_or_0);
    /* 50% duty at 10-bit resolution = 512. */
    ledc_set_duty(IND_BUZZER_MODE, IND_BUZZER_CHANNEL, 512);
    ledc_update_duty(IND_BUZZER_MODE, IND_BUZZER_CHANNEL);
}

void indicator_vibra(bool on) {
    if (!s_ready)
        return;
    gpio_set_level(IND_VIBRA_GPIO, on ? 1 : 0);
}

#else
/* Host/emulator build: indicator_virt.c owns the indicators.h implementation. */
#endif
