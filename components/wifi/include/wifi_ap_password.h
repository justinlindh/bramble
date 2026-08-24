#ifndef BRAMBLE_WIFI_AP_PASSWORD_H
#define BRAMBLE_WIFI_AP_PASSWORD_H

#include <stddef.h>
#include <stdint.h>

/*
 * Per-device SoftAP WPA2 password derivation.
 *
 * The AP password is derived from this node's own identity secret, which
 * makes it unique per device and unknowable to anyone who has not been shown
 * it by the device itself. A build-time constant from a public Kconfig
 * default would instead hand every reader of the repository the PSK for every
 * Bramble AP in the world.
 *
 * Properties this derivation is built for:
 *
 *  - STABLE. It is a pure function of the identity secret, so it survives
 *    reboots and reflashes and never changes under a user who wrote it down.
 *    Nothing is stored: there is no extra NVS state to get out of sync.
 *  - ONE-WAY. HKDF-SHA256 over the identity secret. The password is meant to
 *    be shown to humans, so the derivation must not leak anything about the
 *    key material it comes from; a hash does not run backwards.
 *  - DOMAIN SEPARATED. A dedicated salt and info string, so this output can
 *    never collide with any other key or token derived from the same secret.
 *  - TYPEABLE. WPA2-PSK allows 8 to 63 characters. A 63-character hex string
 *    satisfies the spec and is miserable to copy off a 128x64 OLED, so the
 *    output is 12 characters from a 32-symbol alphabet: 60 bits of entropy,
 *    which is far past any offline-crack concern for a network that only
 *    exists while the device is in the room, at a length a human can read
 *    once and type once.
 *  - UNAMBIGUOUS. The alphabet drops the glyph pairs that get misread on a
 *    small mono font: no 'l', 'i' or 'o' (against '1' and '0'), and no
 *    uppercase at all, so there is no I/l or O/0 confusion and no shift key
 *    on a T-Deck thumb keyboard. Exactly 32 symbols remain, so each output
 *    byte maps through 5 bits with no modulo bias.
 */

/* Password length in characters, excluding the NUL. */
#define WIFI_AP_PASSWORD_LEN 12

/* Minimum buffer for a derived password (length + NUL). */
#define WIFI_AP_PASSWORD_BUFSZ (WIFI_AP_PASSWORD_LEN + 1)

/* 32 unambiguous symbols: no 'i', 'l', 'o', no uppercase. */
#define WIFI_AP_PASSWORD_ALPHABET "023456789abcdefghjkmnpqrstuvwxyz"

/* Domain separation for this derivation. Bump the version suffix if the
 * output format ever changes, which would rotate every device's password. */
#define WIFI_AP_PASSWORD_HKDF_SALT "bramble-softap-psk-v1"
#define WIFI_AP_PASSWORD_HKDF_INFO "softap-password"

/* WPA2-PSK passphrase bounds, for validating an explicit override. */
#define WIFI_AP_PASSWORD_MIN 8
#define WIFI_AP_PASSWORD_MAX 63

/*
 * Derive this device's AP password from secret identity material.
 *
 * secret must be key material that is not published anywhere: the node
 * address and public keys are broadcast in beacons, so deriving from those
 * would hand the PSK to anyone within radio range. The firmware passes the
 * Ed25519 identity private key.
 *
 * Writes exactly WIFI_AP_PASSWORD_LEN characters plus a NUL. Returns 0 on
 * success, -1 on bad arguments or an HKDF failure (leaving out as an empty
 * string, never a partial or guessable password).
 */
int wifi_ap_password_derive(const uint8_t* secret, size_t secret_len, char* out, size_t out_len);

#endif /* BRAMBLE_WIFI_AP_PASSWORD_H */
