#ifndef BRAMBLE_CHANNEL_STORAGE_H
#define BRAMBLE_CHANNEL_STORAGE_H

#include "channel_key.h"
#include "channel_msg.h"  /* for MAX_CHANNELS */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Initialize channel storage subsystem.
 * Call once at boot (no-op on ESP, required for host tests).
 * Returns 0 on success, -1 on error.
 */
int channel_storage_init(void);

/**
 * Save channel list to NVS.
 * Stores channel blobs, names, and default channel index.
 * Overwrites previous data.
 * 
 * @param channels Array of channel structures
 * @param num_channels Number of channels (0-MAX_CHANNELS)
 * @param names Array of channel names (20 chars each)
 * @param default_channel_idx Default channel index (for unicast)
 * @return 0 on success, -1 on error
 */
int channel_storage_save(const bramble_channel_t *channels, int num_channels,
                         const char names[][20], int default_channel_idx);

/**
 * Load channel list from NVS.
 * 
 * @param channels Output array for channel structures
 * @param num_channels Output: number of channels loaded
 * @param names Output array for channel names
 * @param default_channel_idx Output: default channel index
 * @return 0 on success, -1 if no data or error
 * Sets *num_channels to 0 if no channels stored.
 */
int channel_storage_load(bramble_channel_t *channels, int *num_channels,
                         char names[][20], int *default_channel_idx);

/**
 * Clear all stored channels from NVS.
 */
void channel_storage_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* BRAMBLE_CHANNEL_STORAGE_H */
