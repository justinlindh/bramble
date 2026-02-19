#ifndef BRAMBLE_BOARD_CONFIG_H
#define BRAMBLE_BOARD_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* Guard ESP-IDF specific includes for host builds */
#ifdef ESP_PLATFORM
#include "driver/spi_master.h"
#include "driver/gpio.h"
#else
/* Host build stubs */
typedef int spi_host_device_t;
typedef int gpio_num_t;
#define SPI2_HOST 1
#endif

/* Capability flags — boards set these to declare what they support */
#define BOARD_CAP_DISPLAY_SSD1306   (1 << 0)
#define BOARD_CAP_DISPLAY_ST7789    (1 << 1)
#define BOARD_CAP_KEYBOARD          (1 << 2)
#define BOARD_CAP_TRACKBALL         (1 << 3)
#define BOARD_CAP_GPS               (1 << 4)
#define BOARD_CAP_SDCARD            (1 << 5)
#define BOARD_CAP_AUDIO             (1 << 6)
#define BOARD_CAP_BATTERY_ADC       (1 << 7)
#define BOARD_CAP_SHARED_SPI        (1 << 8)  /* SPI bus shared with display/SD */
#define BOARD_CAP_PERIPHERAL_POWER  (1 << 9)  /* Needs power pin enabled first */

/* Radio oscillator type */
typedef enum {
    RADIO_OSC_TCXO_DIO3 = 0,   /* TCXO controlled by DIO3 (e.g., Heltec V3) */
    RADIO_OSC_CRYSTAL,          /* Crystal oscillator (e.g., T-Deck Plus) */
} radio_osc_type_t;

/* Radio regulator type */
typedef enum {
    RADIO_REG_DCDC = 0,     /* DC-DC converter */
    RADIO_REG_LDO,          /* LDO regulator */
} radio_reg_type_t;

/* SPI pin config */
typedef struct {
    int mosi;
    int miso;
    int sck;
} board_spi_pins_t;

/* Radio pin config */
typedef struct {
    int cs;     /* NSS/chip select */
    int rst;    /* Reset */
    int busy;   /* Busy indicator */
    int dio1;   /* DIO1 interrupt */
} board_radio_pins_t;

/* Display pin config (SPI displays) */
typedef struct {
    int cs;
    int dc;
    int backlight;
} board_display_spi_pins_t;

/* Display pin config (I2C displays) */
typedef struct {
    int sda;
    int scl;
    int rst;
    int vext;       /* Power gate pin (-1 if none) */
    uint8_t addr;   /* I2C address */
} board_display_i2c_pins_t;

/* Battery ADC config */
typedef struct {
    int gpio;
    int adc_channel;    /* ADC channel number */
    int divider_factor; /* Voltage divider multiplier (e.g., 2) */
} board_battery_config_t;

/* Trackball pin config */
typedef struct {
    int up;
    int down;
    int left;
    int right;
    int center;
} board_trackball_pins_t;

/* GPS UART config */
typedef struct {
    int tx;
    int rx;
    int baud;
} board_gps_config_t;

/* Full board configuration */
typedef struct {
    const char *name;           /* Human-readable name */
    const char *short_name;     /* For logs (e.g., "heltec_v3") */
    uint32_t capabilities;      /* Bitmask of BOARD_CAP_* flags */

    /* Power */
    int peripheral_power_pin;   /* GPIO to enable peripherals (-1 if none) */

    /* SPI */
    board_spi_pins_t spi;
    spi_host_device_t spi_host;
    int spi_max_transfer_sz;    /* Max DMA transfer size */

    /* Radio */
    board_radio_pins_t radio;
    radio_osc_type_t radio_osc;
    float radio_tcxo_voltage;   /* Only used if radio_osc == TCXO_DIO3 */
    radio_reg_type_t radio_reg;

    /* Display */
    uint16_t display_width;
    uint16_t display_height;
    union {
        board_display_spi_pins_t spi_display;
        board_display_i2c_pins_t i2c_display;
    };

    /* Button */
    int button_gpio;            /* Single button GPIO (-1 if none) */

    /* Battery */
    board_battery_config_t battery;

    /* I2C bus (for keyboard, trackball, sensors) */
    int i2c_sda;
    int i2c_scl;

    /* Keyboard interrupt GPIO */
    int keyboard_int;  /* Keyboard interrupt GPIO (-1 if none) */

    /* Trackball */
    board_trackball_pins_t trackball;

    /* GPS */
    board_gps_config_t gps;

    /* SD card CS pin */
    int sdcard_cs;
} bramble_board_config_t;

/**
 * Get the board configuration for the current build target.
 * Returns a pointer to a static const struct.
 * Implementation provided by main/board.c
 */
const bramble_board_config_t *board_get_config(void);

/**
 * Board-level initialization: power pins, shared SPI bus, etc.
 * Must be called first in app_main, before any peripheral init.
 * Returns 0 on success.
 * Implementation provided by main/board.c
 */
int board_init(void);

/**
 * Check if board has a specific capability.
 */
static inline bool board_has_cap(uint32_t cap) {
    return (board_get_config()->capabilities & cap) != 0;
}

#endif /* BRAMBLE_BOARD_CONFIG_H */
