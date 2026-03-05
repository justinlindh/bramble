#ifndef BRAMBLE_KEY_BACKUP_H
#define BRAMBLE_KEY_BACKUP_H

#include <stdint.h>
#include <stdbool.h>

// Backup state machine
typedef enum {
    BACKUP_IDLE = 0,   // No backup in progress
    BACKUP_REQUESTED,  // BLE client requested backup, waiting for button
    BACKUP_AUTHORIZED, // Button pressed, backup ready to send
    BACKUP_SENDING,    // Transmitting key material over BLE
    BACKUP_COMPLETE,   // Done
    BACKUP_TIMEOUT,    // User didn't press button within window
} backup_state_t;

#define BACKUP_AUTH_TIMEOUT_MS 30000 // 30 seconds to press button
#define BACKUP_KEY_MATERIAL_SIZE 68  // privkey(32) + pubkey(32) + addr(4)

typedef struct {
    backup_state_t state;
    uint32_t request_time;
    bool button_pressed;
} key_backup_ctx_t;

void key_backup_init(key_backup_ctx_t* ctx);

// Called when BLE client sends backup request
// Returns 0 if request accepted (moves to BACKUP_REQUESTED), -1 if busy
int key_backup_request(key_backup_ctx_t* ctx, uint32_t now_ms);

// Called when physical button is pressed
// Returns 0 if authorized (moves to BACKUP_AUTHORIZED), -1 if no pending request
int key_backup_authorize(key_backup_ctx_t* ctx, uint32_t now_ms);

// Check for timeout
void key_backup_tick(key_backup_ctx_t* ctx, uint32_t now_ms);

// Get current state
backup_state_t key_backup_get_state(const key_backup_ctx_t* ctx);

// Prepare export payload (only works in AUTHORIZED state)
// Fills output buffer with encrypted key material
// encryption_key is derived from a user-provided passphrase via HKDF
int key_backup_export(key_backup_ctx_t* ctx, const uint8_t* identity_privkey,
                      const uint8_t* identity_pubkey, uint32_t node_addr,
                      const uint8_t* encryption_key, uint8_t* out, size_t out_max, size_t* out_len);

// Import/restore from backup (for a fresh node)
int key_backup_import(const uint8_t* encrypted_backup, size_t backup_len,
                      const uint8_t* encryption_key, uint8_t* privkey_out, uint8_t* pubkey_out,
                      uint32_t* addr_out);

#endif
