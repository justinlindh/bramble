// Access to the mounted settings filesystem for the other nRF-target
// consumers (the message store). The NVS shim owns the mount so there is
// exactly one lfs_t and one partition on this device.
#pragma once

#include "lfs.h"

// Returns the mounted filesystem, or NULL when nvs_flash_init has not
// succeeded (callers must treat NULL as "no persistence").
lfs_t* nvs_lfs_handle(void);
