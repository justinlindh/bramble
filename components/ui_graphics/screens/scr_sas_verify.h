#ifndef SCR_SAS_VERIFY_H
#define SCR_SAS_VERIFY_H

#include "scr_layout.h"
#include <stdint.h>

/* Pager SAS verification screen (DM forward-secrecy + SAS): shows the
 * grouped 7-digit identity safety number for peer_addr and, on confirm, marks
 * the peer verified via mesh_set_peer_verified. */
void scr_sas_verify_open(bramble_layout_t* layout, uint32_t peer_addr);

#endif
