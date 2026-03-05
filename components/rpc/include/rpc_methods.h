#ifndef BRAMBLE_RPC_METHODS_H
#define BRAMBLE_RPC_METHODS_H

#include "identity.h"

/**
 * Register all bramble.* RPC methods with the dispatcher.
 * Call after rpc_init() and mesh_task_start().
 */
void rpc_methods_init(bramble_identity_t* identity);

#endif
