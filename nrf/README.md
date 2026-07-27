# Bramble nRF52840 target (P0 bring-up)

Experimental scaffold for porting Bramble to the nRF52840 (Seeed Wio-WM1110
dev kit now, SenseCAP T1000-E later). Bare-metal FreeRTOS + nrfx, no ESP-IDF,
no SoftDevice.

Status: P0 bring-up in progress. Builds and boots on the Wio-WM1110 dev kit
bench. No radio, no flash persistence, not a supported device yet.

This README is finalized at the end of P0 with build, flash, console, and
debug recipes.

## Measured memory (P0 exit gate)

Measured 2026-07-27 on the full P0 image (portable protocol stack + crypto +
FreeRTOS + null radio) at commit `147f4c4d`, via `scripts/size_report.py`
(runs on every build and fails it over budget):

| Item | Bytes | Notes |
|---|---|---|
| .bss | 117,580 | includes 48KB FreeRTOS heap (`ucHeap`) |
| .data | 112 | |
| libc heap (.heap) | 16,388 | nrfx startup default, newlib only |
| MSP stack | 16,384 | nrfx startup default |
| RAM total | 150,464 / 262,144 | 57.4%, 54,336 under the 200KB gate |
| Flash | 43,592 / 1,048,576 | 4.2% |

These measurements supersede the scoping spike's 190-230KB estimate. Largest
static objects: `ucHeap` 48K, `s_dm_table` 44K (the DM session table, the
top shrink knob if P2 needs room), `s_msgs_storage` 14K. The libc heap and
MSP stack sizes are untuned nrfx defaults with obvious headroom to reclaim.
Not yet in this image: LR1110 driver + radio buffers (P1), NimBLE (~30K, P2),
and mesh_task statics from `main/` (P2); the 54KB slack is their landing
zone, which is why the gate does not move.
