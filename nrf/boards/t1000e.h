// Pin map for the Seeed SenseCAP T1000-E card tracker (nRF52840 + LR1110).
// Source: Meshtastic variants/nrf52840/tracker-t1000-e (variant.h and
// rfswitch.h), the field-proven community map; Seeed publishes no full
// schematic. Pins on port 1 are written (32 + n).
//
// Differences from the dev kit that matter here: every LoRa bus pin moves
// (SCK even changes ports), the RF switch is a FOUR pin network (DIO8 joins
// DIO5/DIO6, and DIO7 drives the GNSS LNA), and there is no USB-UART bridge,
// so the console is compiled out and BLE is the only interface. Two pins
// swap roles between the boards in ways that would misfire if the dev kit
// map were reused: the dev kit's 3V3_EN (P0.07) is this board's radio BUSY
// line, and the dev kit's GNSS antenna enable (P1.05) is this board's buzzer
// enable.
#pragma once

// LED and button. P0.24 is the green status LED (active high). The button
// is active HIGH with an external pull-down.
#define BOARD_PIN_LED1 24  // P0.24, green LED
#define BOARD_PIN_BUTTON 6 // P0.06, active high

// No console: the T1000-E has no USB-UART bridge, and the only exposed UART
// pads (P0.13/P0.14) belong to the AG3335 GNSS. Driving them would talk
// over the GNSS serial port.
#define BOARD_HAS_CONSOLE 0
// The pins are still named so console_uart.c compiles; with
// BOARD_HAS_CONSOLE 0 it never initializes the peripheral or touches them.
#define BOARD_PIN_CONSOLE_TX 13 // P0.13, see BOARD_PIN_GNSS_TX (AG3335 RX)
#define BOARD_PIN_CONSOLE_RX 14 // P0.14, see BOARD_PIN_GNSS_RX (AG3335 TX)

// AG3335 GNSS (Meshtastic tracker-t1000-e variant, the field-proven map).
// P0.13/P0.14 are the same pads named in the console stanza above: the
// console never drives them (BOARD_HAS_CONSOLE 0); the GNSS driver does.
#define BOARD_HAS_GNSS 1
#define BOARD_PIN_GNSS_TX 13           // P0.13, nRF TX -> AG3335 RX
#define BOARD_PIN_GNSS_RX 14           // P0.14, nRF RX <- AG3335 TX
#define BOARD_GNSS_BAUD 115200
#define BOARD_PIN_GNSS_EN (32 + 11)    // P1.11, HIGH = GNSS power on
#define BOARD_PIN_GNSS_RESET (32 + 15) // P1.15, idle LOW, pulse HIGH to reset
#define BOARD_PIN_GNSS_VRTC_EN 8       // P0.08, backup power: set HIGH once, NEVER clear
#define BOARD_PIN_GNSS_SLEEP_INT (32 + 12) // P1.12, hold HIGH
#define BOARD_PIN_GNSS_RTC_INT 15      // P0.15, LOW; pulse HIGH to wake
#define BOARD_PIN_GNSS_RESETB (32 + 14) // P1.14, reset status readback, input pull-up

// LR1110 radio bus
#define BOARD_PIN_LORA_SCK 11          // P0.11
#define BOARD_PIN_LORA_MOSI (32 + 9)   // P1.09
#define BOARD_PIN_LORA_MISO (32 + 8)   // P1.08
#define BOARD_PIN_LORA_NSS 12          // P0.12
#define BOARD_PIN_LORA_RESET (32 + 10) // P1.10
#define BOARD_PIN_LORA_IRQ (32 + 1)    // P1.01, DIO9 IRQ line
#define BOARD_PIN_LORA_BUSY 7          // P0.07

// Battery ADC and charge detect. Verified 2026-08-01 against
// github.com/meshtastic/firmware variants/nrf52840/tracker-t1000-e/variant.h
// (BATTERY_PIN, ADC_MULTIPLIER, VBAT_AR_INTERNAL/AREF_VOLTAGE, ADC_RESOLUTION,
// EXT_CHRG_DETECT, EXT_PWR_DETECT) and src/Power.cpp (the pin modes and the
// isCharging()/isVbusIn() polarity, since variant.h alone does not say how
// the detect pins are configured).
//
// BOARD_PIN_VBAT_ADC currently reads a DEAD divider: the divider hangs off a
// sensor rail this port does not power, so conversions succeed and return 1
// to 4 mV. Do NOT add a define for the rail gate (P1.06, SENSE_POWER_EN per
// Seeed's vendor SDK) to make it read: driving that pin high stopped the
// board within about a millisecond on hardware. See nrf/shim/battery_saadc.c.
#define BOARD_HAS_BATTERY 1
#define BOARD_PIN_VBAT_ADC 2           // P0.02 = AIN0, BAT_ADC (reads a dead divider)
#define BOARD_BATTERY_DIVIDER 2        // ADC_MULTIPLIER 2.0F
#define BOARD_PIN_CHRG_DETECT (32 + 3) // P1.03, EXT_CHRG_DETECT, active LOW = charging
#define BOARD_PIN_VBUS_DETECT 5        // P0.05, EXT_PWR_DETECT, HIGH = external power present

// RF switch truth table (Meshtastic rfswitch.h for this board):
//   mode    DIO5 DIO6 DIO7 DIO8
//   STBY     0    0    0    0
//   RX       1    0    0    1
//   TX       1    1    0    1
//   TX_HP    0    1    0    1
//   GNSS     0    0    1    0
// RFSW0..RFSW3 are the LR1110 names for DIO5..DIO8.
#define BOARD_RFSW_ENABLE                                                                          \
    (LR11XX_SYSTEM_RFSW0_HIGH | LR11XX_SYSTEM_RFSW1_HIGH | LR11XX_SYSTEM_RFSW2_HIGH |              \
     LR11XX_SYSTEM_RFSW3_HIGH)
#define BOARD_RFSW_STANDBY 0
#define BOARD_RFSW_RX (LR11XX_SYSTEM_RFSW0_HIGH | LR11XX_SYSTEM_RFSW3_HIGH)
#define BOARD_RFSW_TX                                                                              \
    (LR11XX_SYSTEM_RFSW0_HIGH | LR11XX_SYSTEM_RFSW1_HIGH | LR11XX_SYSTEM_RFSW3_HIGH)
#define BOARD_RFSW_TX_HP (LR11XX_SYSTEM_RFSW1_HIGH | LR11XX_SYSTEM_RFSW3_HIGH)
#define BOARD_RFSW_GNSS LR11XX_SYSTEM_RFSW2_HIGH
