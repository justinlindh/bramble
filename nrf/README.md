# Bramble nRF52840 target (P0 bring-up)

Experimental scaffold for porting Bramble to the nRF52840 (Seeed Wio-WM1110
dev kit now, SenseCAP T1000-E later). Bare-metal FreeRTOS + nrfx, no ESP-IDF,
no SoftDevice.

Status: P0 bring-up in progress. Builds and boots on the Wio-WM1110 dev kit
bench. No radio, no flash persistence, not a supported device yet.

This README is finalized at the end of P0 with build, flash, console, and
debug recipes plus measured memory numbers.
