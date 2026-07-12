/*
 * Bramble SAR ADC oneshot stub (P2.3b).
 * See hw/xtensa/bramble_adc.c.
 */

#ifndef HW_XTENSA_BRAMBLE_ADC_H
#define HW_XTENSA_BRAMBLE_ADC_H

#include "exec/memory.h"

/*
 * Attach the Bramble SAR ADC oneshot stub to the running esp32s3 machine.
 * Overlays the (unmodeled) SENS peripheral window so the IDF oneshot driver's
 * "conversion done" poll completes instead of spinning forever, letting
 * battery_read_mv() return and app_main reach the main loop.
 *
 *   sys_mem  the system address space the SENS region lives in.
 *
 * Takes no interrupt line: the pager reads the ADC via the polling
 * adc_oneshot_read path (adc_oneshot_ll_get_event), never the ADC ISR, so a
 * done-event register bit is all that is needed. Called from
 * esp32s3_machine_init() alongside bramble_gpio_attach / bramble_gpspi2_attach.
 */
void bramble_adc_attach(MemoryRegion *sys_mem);

#endif
