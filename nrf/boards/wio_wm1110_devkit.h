// Pin map for the Seeed Wio-WM1110 Dev Kit (nRF52840 + LR1110).
// Source: Meshtastic variants/nrf52840/wio-sdk-wm1110/variant.h, the
// authoritative community pin map for this module (Seeed publishes no
// schematic for the WM1110 itself). Pins on port 1 are written (32 + n).
#pragma once

// LEDs and button
#define BOARD_PIN_LED1 13   // P0.13, user LED
#define BOARD_PIN_LED2 14   // P0.14
#define BOARD_PIN_BUTTON 23 // P0.23

// UARTs. UART1 reaches the on-board CH340 USB bridge: determined on the
// bench 2026-07-27 by flashing each pair and capturing the banner on the
// CH340 host port (UART2 was silent, UART1 printed).
#define BOARD_PIN_UART1_TX 24 // P0.24, console TX to CH340
#define BOARD_PIN_UART1_RX 22 // P0.22, console RX from CH340
#define BOARD_PIN_UART2_TX 8  // P0.08
#define BOARD_PIN_UART2_RX 6  // P0.06
#define BOARD_PIN_CONSOLE_TX BOARD_PIN_UART1_TX
#define BOARD_PIN_CONSOLE_RX BOARD_PIN_UART1_RX
#define BOARD_HAS_CONSOLE 1

#define BOARD_HAS_GNSS 0

// LR1110 radio bus (P1 phase; unused in P0)
#define BOARD_PIN_LORA_SCK (32 + 13)   // P1.13
#define BOARD_PIN_LORA_MOSI (32 + 14)  // P1.14
#define BOARD_PIN_LORA_MISO (32 + 15)  // P1.15
#define BOARD_PIN_LORA_NSS (32 + 12)   // P1.12
#define BOARD_PIN_LORA_RESET (32 + 10) // P1.10
#define BOARD_PIN_LORA_IRQ (32 + 8)    // P1.08, DIO1
#define BOARD_PIN_LORA_BUSY (32 + 11)  // P1.11, DIO2
#define BOARD_PIN_GNSS_ANT (32 + 5)    // P1.05, LR1110 GNSS antenna enable

// Peripheral power
#define BOARD_PIN_3V3_EN 7 // P0.07, sensor rail enable

// RF switch truth table (Seeed vendor SDK, Meshtastic-confirmed): a two pin
// network on DIO5/DIO6. RFSW0/RFSW1 are the LR1110 names for DIO5/DIO6.
#define BOARD_RFSW_ENABLE (LR11XX_SYSTEM_RFSW0_HIGH | LR11XX_SYSTEM_RFSW1_HIGH)
#define BOARD_RFSW_STANDBY 0
#define BOARD_RFSW_RX LR11XX_SYSTEM_RFSW0_HIGH
#define BOARD_RFSW_TX (LR11XX_SYSTEM_RFSW0_HIGH | LR11XX_SYSTEM_RFSW1_HIGH)
#define BOARD_RFSW_TX_HP LR11XX_SYSTEM_RFSW1_HIGH
#define BOARD_RFSW_GNSS 0
