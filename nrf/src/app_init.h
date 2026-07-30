#pragma once

// Bring up the portable Bramble stack (see app_init.c). Call from a task
// with at least 2KB of stack: identity generation runs mbedtls ECP.
void app_init_stack(void);
