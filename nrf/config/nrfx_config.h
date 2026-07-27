// nrfx peripheral enable set for the Bramble nRF52840 target. Only what the
// firmware actually uses is enabled; everything else stays at the template
// defaults (off).
//
// The guard macro must be the literal NRFX_CONFIG_H__: the nrfx template
// header refuses direct inclusion by checking for it.
#ifndef NRFX_CONFIG_H__
#define NRFX_CONFIG_H__

#define NRFX_UARTE_ENABLED 1
#define NRFX_UARTE0_ENABLED 1
#define NRFX_RNG_ENABLED 1

#include "nrfx_config_nrf52840.h"

#endif // NRFX_CONFIG_H__
