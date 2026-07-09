# T-Deck WDT Crash — 2026-03-02

## Context
During broadcast delivery receipt testing (10 broadcasts, 15s spacing, 15s wait).
T-Deck stopped responding to delivery receipts after test 7 (tests 8-10 missing D4813079).

## Crash Log
```
E (3135232) task_wdt: Task watchdog got triggered. The following tasks/users did not reset the watchdog in time:
E (3135232) task_wdt:  - mesh (CPU 1)
E (3135232) task_wdt: Tasks currently running:
E (3135232) task_wdt: CPU 0: IDLE0
E (3135232) task_wdt: CPU 1: IDLE1
E (3135232) task_wdt: Print CPU 1 backtrace

Backtrace: 0x40379EB2:0x3FCA3520 0x40378681:0x3FCA3540 0x4037D5A7:0x3FCC3150 0x420052FA:0x3FCC3170 0x40383E25:0x3FCC3190 0x40382781:0x3FCC31B0
```

## Likely Cause
`send_broadcast_delivery_receipt()` blocks mesh task with `vTaskDelay()` for:
- Slot delay: up to 6.4s
- Retry delays: 500-2100ms × 2 gaps
- Total potential block: ~10s

Combined with LBT CAD checks (which also briefly take the radio) and T-Deck SPI bus
contention (display + radio share SPI), the mesh task can exceed the WDT timeout.

## Follow-up
- Symbolize backtrace with `xtensa-esp32s3-elf-addr2line`
- Reproduce under controlled conditions
- Fix: move receipt timing off mesh task (timer/queue) or feed WDT during delays
