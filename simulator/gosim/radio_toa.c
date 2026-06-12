/*
 * radio_toa.c: compiles the firmware's LoRa time-on-air calculation
 * (bramble_calculate_airtime_us) as its own translation unit. It cannot live
 * in all.c because radio.h defines a radio_config_t that collides with the
 * simulator engine's radio_config_t in a single TU.
 */
#include "../../components/radio/radio_airtime.c"
