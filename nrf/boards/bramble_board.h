// Board selection for the nRF52840 target. Exactly one BOARD_PIN map is in
// effect per build; nrf/CMakeLists.txt defines BRAMBLE_BOARD_* from the
// BRAMBLE_NRF_BOARD cache option. Everything that needs a pin includes this
// header, never a board header directly.
#pragma once

#if defined(BRAMBLE_BOARD_T1000E)
#include "t1000e.h"
#else
#include "wio_wm1110_devkit.h"
#endif
