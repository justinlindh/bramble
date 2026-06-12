# OTA signing keys

This directory holds the OTA image-signing key material referenced by
`sdkconfig.defaults` (`CONFIG_SECURE_BOOT_SIGNING_KEY="keys/ota_signing_key.pem"`).

Committed:

- `ota-release-pub.pem`: the PUBLIC half of the release signing key. CI
  verifies every built artifact against it (`espsecure.py verify_signature`).

Never committed (gitignored):

- `ota_signing_key.pem`: the PRIVATE key the build signs with.
  - CI writes the release key here from the `OTA_SIGNING_KEY` repo secret.
  - Local builds get a key via `scripts/ensure-ota-signing-key.sh`: it copies
    from `$BRAMBLE_OTA_SIGNING_KEY` if set, otherwise generates a throwaway
    dev key. Dev-signed devices only accept OTA images signed with that same
    dev key; moving a device between dev and release trust domains requires a
    USB flash.

Key handling, rotation, and the trust model are documented in
`docs/design/ota-signing.md`.
