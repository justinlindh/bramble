#ifndef BRAMBLE_RADIO_INTERNAL_H
#define BRAMBLE_RADIO_INTERNAL_H

#include <stdint.h>

/*
 * Private to the radio component. The raw transmit primitive is
 * deliberately NOT in include/radio.h: every transmission must go through
 * the tx_gate chokepoint (budget check -> LBT -> transmit -> ToA debit),
 * so no caller outside this component can put bytes on the air unbudgeted.
 */
int radio_transmit_raw(const uint8_t* data, uint8_t len);

#endif
