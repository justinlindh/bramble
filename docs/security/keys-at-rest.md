# Keys at Rest (SEC-H4)

## Threat

Adversary class: device thief with physical flash access. Assets: identity
private key, channel keys, WS/BLE auth token. Before this work all three were
stored in plaintext NVS (components/identity/identity.c,
components/channel/channel_storage.c) and dumpable with a flash reader.

## What the secure build protects

Building with the secure overlay:

    idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.secure" build

enables flash encryption (development mode) and NVS encryption. The
bramble_id, bramble_ch, and bramble NVS namespaces (identity keys, channel
keys, auth token) become ciphertext; the XTS keys live in the
flash-encryption-protected nvs_keys partition (partitions.secure.csv).

## What it does NOT protect (yet)

- The spiffs message-history partition is not encrypted by this change. Message
  content at rest is tracked separately, not under SEC-H4.
- Development-mode flash encryption uses a regenerable key. Real tamper
  resistance requires the human-gated Secure Boot v2 + release-mode burn.

## Entropy (SEC-L1)

Key generation now runs behind an entropy gate: crypto_random fails closed
until main() enables the bootloader SAR-ADC entropy source
(bootloader_random_enable) before identity generation, then disables it
before Wi-Fi/ADC init (the SAR-ADC is shared with the battery driver). The
gate reopens once Wi-Fi or BLE brings up its own RF-based entropy source, so
crypto_random is only unavailable during the brief pre-RF window at boot, not
permanently after it.

## Migration (one time)

An existing device with plaintext NVS cannot decrypt its old entries under the
new encrypted format. On the first encrypted boot, once the nvs_keys
partition's keys are confirmed valid (read successfully, or generated fresh
on a brand-new keys partition), if nvs_flash_secure_init still cannot decrypt
the existing NVS contents with that key, the firmware treats this as the
genuine plaintext-to-encrypted migration: it erases NVS and re-initializes
it. The device regenerates its identity and channel set and MUST BE
RE-PAIRED. This is a deliberate, acceptable one-time reset: the fleet is
first-party and pre-alpha.

A failure at the keys layer itself (missing nvs_keys partition, or a failed
read/generate of its keys, e.g. a corrupt keys partition) is NOT treated as a
migration and does NOT erase anything. The firmware aborts boot instead:
erasing on that signal could wipe a healthy device on a transient fault, or
repeatedly wipe it every boot if the fault persisted.

## Human-gated activation

Enabling flash encryption BURNS eFuses on first boot and is IRREVERSIBLE. The
overlay ships inert. A human runs the secure build/flash on the sacrificial
board only (USER DECISION D4) after bench validation. Release-mode keys are
handled outside the repo.
