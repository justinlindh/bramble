# Bramble: rules and hard-won knowledge for AI sessions

House rules (violations block CI or the pre-tool hook):
- NO em dashes anywhere: code, comments, commits, PR bodies. A hook rejects them.
- NO AI attribution in commits or PRs (no Co-Authored-By, no session links).
- Conventional commit style; branch names must match fix/** feat/** feature/** chore/** ci/** or CI never runs.
- clang-format: format with the CI runner's v14, not the local binary:
  `docker run --rm -v "$PWD:/w" -w /w bramble/runner-full:22.04-go126 clang-format -i <files>`

Gates (run with REAL exit codes; `cmd | grep ...` hides failures behind grep's status):
- Firmware: `bash test/run_all_tests.sh`; board builds via `bash scripts/flash.sh local <board> build`.
- Webapp: cd webapp; `npm run lint` (= typecheck), `npm run test:unit`, `npm run build`.
- CI parity: `make ci`. Packaging: `make package-linux|package-android|package-win`.

Bench hardware (identify nodes by ADDRESS, ports renumber on replug):
- V4  F2BE6EEE (heltec_v4, GPS L76K breakout) and V4B FEC61437 (heltec_v4, GPS): plaintext flash.
- V3  AB246C7C (heltec_v3, no GPS, CP2102 on ttyUSB*): flash-encryption eFuse is BURNED.
  ALWAYS flash it app-only with `--encrypt 0x10000 build-heltec-v3/bramble.bin` or it bricks
  and loses its NVS identity. Never write bootloader/partition table to it casually.
- Fleet flashing: `bash scripts/flash-fleet.sh [build]` applies the rule automatically.
- Serial RPC: `scripts/bramble-rpc PORT METHOD [PARAMS_JSON]` (run under
  `~/.local/share/pipx/venvs/esptool/bin/python`; --cp2102 for ttyUSB ports).
- USB-JTAG ports (ttyACM*): opens HANG if the chip is wedged; wrap in `timeout`, use
  exclusive=True, and recover with `esptool --before default_reset read_mac`.
- "ANTENNA OPEN" from the L76K GPS is permanent cosmetic noise with its passive ceramic
  patch antenna; a valid fix is the ground truth.
- The GPS "add-on" ceramic square IS the antenna. LoRa (915MHz) and GPS (1575MHz) antennas
  are not interchangeable.

Process gotchas that have burned sessions:
- Verify a PR is merged (merged:true) BEFORE deleting its branch: deleting first auto-closes
  the PR unmerged (Gitea re-checks mergeability when the base moves).
- Never `pkill -f PATTERN` where PATTERN appears in your own command line; kill by saved PID
  or by port (`fuser -k PORT/tcp`).
- Before shipping an APK or package, grep the BUILT artifact for a string unique to the new
  change; asset syncs silently no-op when the webapp worktree is on the wrong commit.
- Multi-edit python scripts against source files: write the file after each independent edit;
  a late assert silently discards earlier edits.
- ESP-IDF CMake runs twice; component requirements must be unconditional or the early pass
  registers stubs (symptom: "add X to PRIV_REQUIRES" persists after adding X).
- clangd diagnostics on firmware files (-mlongcalls, machine/endian.h) are cross-compile
  noise; trust the real builds.

Packaged-app verification recipes:
- Electron: xvfb screenshots come back BLACK (compositing artifact). Verify via CDP:
  launch with `--remote-debugging-port`, curl /json, Runtime.evaluate. requestDevice/
  requestPort need `userGesture: true` in Runtime.evaluate. select-serial-port is a
  SESSION event; select-bluetooth-device is a WEBCONTENTS event.
- Android emulator (AVD bramble-e2e): connect the in-page mock node, background with
  KEYCODE_HOME before expecting notifications, `dumpsys notification --noredact` for proof.
- A BLE node serves ONE central at a time and stops advertising while held; a phone's
  auto-reconnect will steal the node from bench tests.

Secrets and enrollment (bramble-meta is a plain directory, NOT a repo; secrets stay local):
- Gitea PAT: ~/src/bramble-meta/secrets/gitea-pat. Fleet netkey + anchor seed live there too.
- New-node enrollment recipe over serial: getIdentity; sign
  b'bramble-endorse-v1' + ed25519_pub + b'\xff'*8 with the anchor seed;
  setAnchor {anchor_pubkey}; setEndorsement {not_after:"ffffffffffffffff", endorsement_sig};
  setNetworkKey {key}. Serial is full-privilege by design; USB needs no auth token.

GitHub mirrors (private, Gitea stays canonical, NO auto-sync):
- Refresh: `git push --force github 'refs/remotes/origin/*:refs/heads/*' && git push --force github --tags`
