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
#define NRFX_UARTE1_ENABLED 1 // AG3335 GNSS (T1000-E); console owns instance 0
// LR1110: SPI transport on SPIM2 (8MHz ceiling dodges the SPIM3 anomaly-198
// question entirely) and the DIO IRQ line via GPIOTE.
#define NRFX_SPIM_ENABLED 1
#define NRFX_SPIM2_ENABLED 1
#define NRFX_GPIOTE_ENABLED 1
#define NRFX_GPIOTE0_ENABLED 1
// Internal flash for the settings/message filesystem.
#define NRFX_NVMC_ENABLED 1

#include "nrfx_config_nrf52840.h"

#endif // NRFX_CONFIG_H__
