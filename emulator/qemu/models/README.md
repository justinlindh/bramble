# Bramble device models (QEMU C sources)

Source files here get injected into the espressif/qemu tree by
`bootstrap-qemu.sh`: `*.c` lands in `hw/xtensa/` (next to `esp32s3.c`) and
`*.h` lands in `include/hw/xtensa/` (next to its sibling headers), matching
where the rest of the esp32s3 machine's own model files already live.
`patches/` wires each new file into the meson build and the esp32s3 machine
init; see `patches/README.md` for what each patch touches.

`bramble_scaffold.c` / `.h` (P2.2-infra) is not a device model: it is the
minimal proof that a file dropped here reaches the running VM (a log line at
machine realize). P2.2-P2.5 land the real models in this same directory:

- P2.2: GPIO model (buttons, GNSS_EN, LED/buzzer/vibra outputs, boot straps)
- P2.3: GPSPI2 controller model (the spi_master register/DMA surface)
- P2.4: SX1262 slave model + shim (SSI bus attach, emu-link bridge)
- P2.5: SSD1680 slave model (BW RAM, Master Activation, BUSY timing)

See `emulator/PHASE2.md` for the full spec and `emulator/qemu/README.md` for
the build/run instructions.
