#pragma once

#include "identity.h"

/**
 * Register all bramble.* RPC method handlers.
 * Call once after rpc_init().
 */
void rpc_methods_init(bramble_identity_t *identity);
