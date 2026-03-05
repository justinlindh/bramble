#ifndef BRAMBLE_SDCARD_H
#define BRAMBLE_SDCARD_H

#include <stdbool.h>

/**
 * Initialize SD card and mount as FAT filesystem.
 * Uses shared SPI bus from board_init().
 * @return 0 on success, -1 on failure (no card, mount failed, etc.)
 */
int sdcard_init(void);

/**
 * Check if SD card is present and mounted.
 * @return true if card is mounted
 */
bool sdcard_is_present(void);

/**
 * Get SD card mount point.
 * @return mount point string (e.g., "/sdcard"), or NULL if not mounted
 */
const char* sdcard_get_mount_point(void);

/**
 * Unmount SD card and free resources.
 */
void sdcard_deinit(void);

#endif /* BRAMBLE_SDCARD_H */
