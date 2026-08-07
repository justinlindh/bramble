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

// Battery voltage. The cell reaches P0.02/AIN0 through a 2x divider, and
// the divider hangs off a sensor rail gated by P1.06: with the gate low the
// SAADC converts successfully but reads the dead divider (single-digit mV),
// a confident wrong answer, so every read must energize the gate first and
// release it after. Sources, in evidence order:
//   - Vendor SDK (Seeed-Tracker-T1000-E-for-LoRaWAN-dev-board,
//     smtc_hal_config.h): SENSE_POWER_EN = 38 (P1.06), SENSE_ADC_BAT = 2
//     (P0.02, AIN0); sensor.c's sensor_bat_sample() drives SENSE_POWER_EN
//     high (push-pull, standard drive, hal_gpio_init_out ->
//     nrf_gpio_cfg_output), samples, multiplies by 2, drives it low again.
//   - Meshtastic variant.h for this board: BATTERY_PIN 2, ADC_MULTIPLIER
//     2.0F, PIN_3V3_EN (32+6) "Power to Sensors", driven high at boot in
//     initVariant() fleet-wide. Its T1000X_SENSOR_EN_PIN (P0.04) is NOT a
//     gate: the vendor SDK names P0.04 SENSE_ADC_VCC, the rail's own ADC
//     sense input (driving it changes nothing, bench-verified 2026-08-06).
//   - Bench probe on this exact unit (2026-08-06): rail off = 2 mV at the
//     pin, P1.06 driven high = 3920 mV on a charging cell, released = 2 mV.
#define BOARD_PIN_VBAT_ADC 2            // P0.02 = AIN0, SENSE_ADC_BAT
#define BOARD_PIN_VBAT_RAIL_EN (32 + 6) // P1.06, SENSE_POWER_EN, HIGH = divider live
// Declares that reading BOARD_PIN_VBAT_ADC without driving the rail returns
// a dead divider. Backends that never drive the gate (battery_null.c)
// refuse at compile time to be built for a board carrying this, because the
// shim is selected by filename in nrf/CMakeLists.txt and a wrong selection
// would otherwise compile clean and misread on hardware.
#define BOARD_BATTERY_NEEDS_RAIL_GATE 1

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
