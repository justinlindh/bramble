#ifndef BRAMBLE_OTA_H
#define BRAMBLE_OTA_H

/** Start OTA update from a URL (HTTP or HTTPS). Returns 0 on success. */
int ota_wifi_start(const char *url);

/** Start OTA via BLE (not yet implemented). */
int ota_ble_start(void);

/** Get the label of the running OTA partition (e.g. "app0"). */
const char *ota_get_running_partition(void);

/** Get the firmware version string. */
const char *ota_get_app_version(void);

#endif

