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
#define BOARD_PIN_CONSOLE_TX 13 // P0.13, GNSS TX pad, unused
#define BOARD_PIN_CONSOLE_RX 14 // P0.14, GNSS RX pad, unused

// LR1110 radio bus
#define BOARD_PIN_LORA_SCK 11          // P0.11
#define BOARD_PIN_LORA_MOSI (32 + 9)   // P1.09
#define BOARD_PIN_LORA_MISO (32 + 8)   // P1.08
#define BOARD_PIN_LORA_NSS 12          // P0.12
#define BOARD_PIN_LORA_RESET (32 + 10) // P1.10
#define BOARD_PIN_LORA_IRQ (32 + 1)    // P1.01, DIO9 IRQ line
#define BOARD_PIN_LORA_BUSY 7          // P0.07

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
