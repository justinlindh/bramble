// sdkconfig.h shim for the nRF52840 target: the Kconfig values portable
// components read, with P0 answers. Grows only when a compile error proves a
// component reads a new option.
#pragma once

#define CONFIG_BRAMBLE_BOARD_CUSTOM 1

// Kconfig defaults (matching components/*/Kconfig and main/Kconfig.projbuild)
#define CONFIG_BRAMBLE_RPC_MAX_METHODS 64
#define CONFIG_BRAMBLE_MSG_STORE_CAP 20
// P2: message persistence on LittleFS (values match the Kconfig defaults).
#define CONFIG_BRAMBLE_MSG_PERSIST_ENABLED 1
#define CONFIG_BRAMBLE_MSG_PERSIST_MAX 200
#define CONFIG_BRAMBLE_MSG_PERSIST_ROLLOVER_KEEP_PCT 75
// Region: US-915 (neither CONFIG_BRAMBLE_REGION_EU nor _AU defined).
