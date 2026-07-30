// Mynewt console shim: NimBLE's optional GATT debug dump prints through this.
// The build leaves those paths disabled, so the macro is a no-op rather than
// a route into the blocking UART writer.
#pragma once

#define console_printf(...) ((void)0)
