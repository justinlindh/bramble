#ifndef BRAMBLE_BOARD_CONFIG_H
#define BRAMBLE_BOARD_CONFIG_H

#include <stdint.h>
#include <stdbool.h>

/* Guard ESP-IDF specific includes for host builds. The POSIX/Linux
 * simulator (IDF linux target) defines ESP_PLATFORM but has no SPI/GPIO
 * drivers, so it takes the host branch too. */
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if defined(ESP_PLATFORM) && !defined(CONFIG_IDF_TARGET_LINUX)
#include "driver/spi_master.h"
#include "driver/gpio.h"
#else
/* Host build stubs */
typedef int spi_host_device_t;
typedef int gpio_num_t;
#define SPI2_HOST 1
#endif

/* Capability flags — boards set these to declare what they support */
#define BOARD_CAP_DISPLAY_SSD1306 (1 << 0)
#define BOARD_CAP_DISPLAY_ST7789 (1 << 1)
#define BOARD_CAP_KEYBOARD (1 << 2)
#define BOARD_CAP_TRACKBALL (1 << 3)
#define BOARD_CAP_GPS (1 << 4)
#define BOARD_CAP_SDCARD (1 << 5)
#define BOARD_CAP_AUDIO (1 << 6)
#define BOARD_CAP_BATTERY_ADC (1 << 7)
#define BOARD_CAP_SHARED_SPI (1 << 8)       /* SPI bus shared with display/SD */
#define BOARD_CAP_PERIPHERAL_POWER (1 << 9) /* Needs power pin enabled first */
#define BOARD_CAP_TOUCH (1 << 10)           /* Capacitive touchscreen */
#define BOARD_CAP_IO_EXPANDER (1 << 11)     /* PCA9535 or similar */

/* Radio oscillator type */
typedef enum {
    RADIO_OSC_TCXO_DIO3 = 0, /* TCXO controlled by DIO3 (e.g., Heltec V3) */
    RADIO_OSC_CRYSTAL,       /* Crystal oscillator (e.g., T-Deck Plus) */
} radio_osc_type_t;

/* Radio regulator type */
typedef enum {
    RADIO_REG_DCDC = 0, /* DC-DC converter */
    RADIO_REG_LDO,      /* LDO regulator */
} radio_reg_type_t;

/* SPI pin config */
typedef struct {
    int mosi;
    int miso;
    int sck;
} board_spi_pins_t;

/* Radio pin config */
typedef struct {
    int cs;   /* NSS/chip select */
    int rst;  /* Reset */
    int busy; /* Busy indicator */
    int dio1; /* DIO1 interrupt */
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
    int vext;     /* Power gate pin (-1 if none) */
    uint8_t addr; /* I2C address */
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

/* Audio I2S config */
typedef struct {
    int i2s_ws;   /* Word select (LRCK) */
    int i2s_bck;  /* Bit clock */
    int i2s_dout; /* Data out to speaker */
} board_audio_config_t;

/* Touch controller config */
typedef struct {
    int int_pin;      /* Interrupt GPIO */
    int rst_pin;      /* Reset GPIO (-1 if shared/none) */
    uint8_t i2c_addr; /* I2C address (0x5D or 0x14 for GT911) */
} board_touch_config_t;

/* Full board configuration */
typedef struct {
    const char* name;       /* Human-readable name */
    const char* short_name; /* For logs (e.g., "heltec_v3") */
    uint32_t capabilities;  /* Bitmask of BOARD_CAP_* flags */

    /* Power */
    int peripheral_power_pin; /* GPIO to enable peripherals (-1 if none) */

    /* SPI */
    board_spi_pins_t spi;
    spi_host_device_t spi_host;
    int spi_max_transfer_sz; /* Max DMA transfer size */

    /* Radio */
    board_radio_pins_t radio;
    radio_osc_type_t radio_osc;
    float radio_tcxo_voltage; /* Only used if radio_osc == TCXO_DIO3 */
    radio_reg_type_t radio_reg;
    bool radio_dio2_rf_switch; /* Issue SetDio2AsRfSwitchCtrl(0x9D) at init when the
                                  module's RF switch hangs off DIO2 (e.g. NiceRF LoRa1262) */

    /* Display */
    uint16_t display_width;
    uint16_t display_height;
    union {
        board_display_spi_pins_t spi_display;
        board_display_i2c_pins_t i2c_display;
    };

    /* Button */
    int button_gpio; /* Single button GPIO (-1 if none) */

    /* Battery */
    board_battery_config_t battery;

    /* I2C bus (for keyboard, trackball, sensors) */
    int i2c_sda;
    int i2c_scl;

    /* Keyboard interrupt GPIO */
    int keyboard_int; /* Keyboard interrupt GPIO (-1 if none) */

    /* Trackball */
    board_trackball_pins_t trackball;

    /* Touch */
    board_touch_config_t touch;

    /* GPS */
    board_gps_config_t gps;

    /* SD card CS pin */
    int sdcard_cs;

    /* Audio */
    board_audio_config_t audio;
} bramble_board_config_t;

/**
 * Get the board configuration for the current build target.
 * Returns a pointer to a static const struct.
 * Implementation provided by main/board.c
 */
const bramble_board_config_t* board_get_config(void);

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

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/**
 * Shared SPI bus mutex for boards with BOARD_CAP_SHARED_SPI.
 *
 * On boards where the radio (SX1262) and display (ST7789) share an SPI bus,
 * this mutex prevents interleaving of multi-step SPI command sequences.
 * Without it, display flush transactions can insert between radio command
 * + BUSY-wait cycles, corrupting the SX1262 state machine.
 *
 * Usage pattern:
 *   - Radio: acquire before each command group, release after
 *   - Display: acquire before flush, release after all pixels sent
 *
 * On non-shared-SPI boards, this is NULL and callers must check before use.
 * Created by board_init() when BOARD_CAP_SHARED_SPI is set.
 */
extern SemaphoreHandle_t g_spi_mutex;
#endif

#endif /* BRAMBLE_BOARD_CONFIG_H */
