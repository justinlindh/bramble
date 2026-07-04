#ifndef BRAMBLE_CLI_H
#define BRAMBLE_CLI_H

#include "identity.h"

/**
 * Initialize serial CLI on UART.
 * Registers commands: send, broadcast, peers, status, help
 */
void cli_init(bramble_identity_t* identity);

#endif
