# Update Your Node Over the Air

Last verified: 2026-07-24 (steps checked against the web client source
and a rendered test of the update card, see "How this was verified")

Your node can install new firmware over WiFi, from the web client, without a
USB cable and without a toolchain. This page is the whole procedure. You do
not need the JSON-RPC API, an auth token beyond the one you already use to
connect, or any command-line tool. Operators who want the raw RPC recipes,
a local dev-build origin, or fleet notes want
[ota-rollout.md](ota-rollout.md) instead.

- [Before you start](#before-you-start)
- [Update the node](#update-the-node)
- [Choosing a version: stable and dev](#choosing-a-version-stable-and-dev)
- [Which devices get updates](#which-devices-get-updates)
- [Where updates come from, and why one signed image is refused](#where-updates-come-from-and-why-one-signed-image-is-refused)
- [Going backwards: the rollback floor](#going-backwards-the-rollback-floor)
- [When it does not work](#when-it-does-not-work)
- [How this was verified](#how-this-was-verified)

## Before you start

- The node is joined to your WiFi and the web client is connected to it. If
  you are still at the first-connection step, do
  [getting-started.md](getting-started.md) first.
- The node needs to reach the update server on the internet. A node on an
  isolated network cannot download an image.
- The update ends in a reboot, so the node is off the mesh for the download
  and the restart.

## Update the node

1. Open the **Config** tab in the web client.
2. Scroll to the **Device Management** section and click **Load Device
   Management**. The section is not loaded until you ask for it.
3. Find the **Firmware Update** subsection. It states what the node is
   running right now, for example "Running 1.3.10 (rollback floor 1.3.10).
   Updates are downloaded from the allowlisted origin below and must be
   signed." The **Update origin** field above it is the server the node
   downloads from; leave it alone unless you know you want a different one.
4. If a newer build than the one you are running has been published for your
   device, a badge reads **Update available: `<version>`**.
5. Pick the build you want in the **Version** dropdown. Entries read version,
   channel, and publication date in your locale's format, for example
   `v1.3.10 (stable) 3/2/2026`, newest first. If the release carries release
   notes they appear as a line of text under the dropdown.
6. Click **Update**. Nothing is installed yet: the card asks
   "Install `<version>`? The node reboots when it finishes." with
   **Install** and **Cancel**. Click **Install** to commit.
7. Watch the progress bar. The label under it walks through **Starting
   update**, **Downloading firmware `NN`%**, **Verifying signature**, and
   **Node is rebooting**, with the reminder "Keep this page open. The node
   reboots when it finishes." A brief connection drop mid-install shows
   **Reconnecting** and then carries on.
8. When the node reboots, the card switches to "Checking the node. Waiting
   for it to come back...". It gives the node up to 45 seconds.
9. Done: the card reads **Updated to `<version>`.** with a **Back** button
   that returns you to the version picker. The version it names is the
   version the node actually reports after the reboot, which is the honest
   answer even if it differs from what you picked.

If anything fails, the card shows the reason in red with a **Try again**
button. See [When it does not work](#when-it-does-not-work).

## Choosing a version: stable and dev

Two channels are published:

- **stable**: cut from a released firmware version tag. This is what you
  want.
- **dev**: built from the main branch between releases. Versions look like
  `v1.4.1-dev.2.g3d863a50`. These are development builds, not
  release-tested.

There is no channel switch in the web client. The channel is a property of
the published build itself: the update server stores each build under
`/<channel>/<version>/<board>/`, and the version dropdown prints the channel
in parentheses next to each entry. Choosing a channel means choosing an
entry labeled `(stable)`.

One thing to watch: the **Update available** badge names the newest
published build for your device regardless of channel, so if a dev build is
the newest thing published, the badge names the dev build. Check the channel
in the dropdown before you install.

## Which devices get updates

Firmware is built and published for four boards:

| Board id in the release index | Device |
| --- | --- |
| `heltec-v3` | Heltec WiFi LoRa 32 V3 |
| `heltec-v4` | Heltec WiFi LoRa 32 V4 |
| `tdeck-plus` | LilyGo T-Deck Plus |
| `bramble-pager` | Bramble Pager v1 (custom PCB) |

You do not choose the board. The web client asks the node what hardware it
is and only offers builds published for that board. If nothing has been
published for your device, the card says "No published updates for this node
yet."

Honest state of the public update server as of 2026-07-24: it carries
`heltec-v3`, `heltec-v4`, and `tdeck-plus` artifacts, and the newest
published stable release is v1.3.10. `bramble-pager` is built and published
by the release workflow but has no published artifact yet (the v1 boards are
not built yet either, see [BUILDING.md](BUILDING.md)), so a pager would show
"No published updates for this node yet".

## Where updates come from, and why one signed image is refused

Every Bramble firmware image is signed, and the node checks the signature
before it switches over. The rule that surprises people:

**A node installs only images signed by the same key that signed the
firmware it is currently running.**

In practice:

- A node flashed from an official release, including through the
  [web flasher](https://bramblemesh.org/web-flasher/), takes official
  updates. This is the normal case and it just works.
- A node you flashed with your own build from source is signed with your own
  development key. It will refuse official images, and official nodes refuse
  your images. The install fails with "OTA rejected: image signature
  verification failed (unsigned or not signed by a trusted key)".
- Crossing between those two worlds needs one USB flash. There is no way to
  do it over the air, on purpose: it is what stops anyone else's key from
  becoming trusted on your device.

The node also downloads only from the origin in the **Update origin** field,
which defaults to `https://bramblemesh.org/ota/`. A firmware image cannot
carry its own URL, and the update request the web client sends is a path
under that origin, never an arbitrary address. The trust model and the key
handling behind this are in [design/ota-signing.md](design/ota-signing.md).

## Going backwards: the rollback floor

The node remembers the highest version it has ever booted and refuses
anything below it by default. That floor is shown next to the running
version ("rollback floor 1.3.10").

If you pick an older version anyway, the confirm step changes to
"Install `<version>`? This is older than the node's rollback floor
`<floor>`. The node reboots when it finishes." with an **Allow downgrade**
checkbox. The **Install** button stays disabled until you tick it.
Downgrading also lowers the floor to the version you installed, so the node
is not stranded afterwards.

A hardware-enforced floor, burned into the chip and impossible to undo,
is designed and host-tested but is **not** enabled in any shipped build
(`docs/design/ota-antirollback.md`). On the firmware you are running today,
the floor is software state and the **Allow downgrade** checkbox is enough
to go back.

## When it does not work

**"Could not load the release index from ..." and the "Advanced: install by
artifact path" section springs open.** The web client could not read the
list of published builds, and fell back to offering you the manual path
entry, which is only useful if you know the exact artifact path. In a
browser, the usual cause is that the page and the update server are
different origins and the update server does not send the browser the header
that permits the read (`Access-Control-Allow-Origin`); the request is made
from the page, so the browser blocks it. The packaged
[desktop app](webapp/desktop.md) fetches the list through its own main
process rather than the page, so it is not subject to that rule and works
regardless of the update server's headers. If you are in a browser and
blocked, use the desktop app.

**The install failed with a signature error.** See
[Where updates come from](#where-updates-come-from-and-why-one-signed-image-is-refused).
This node and that image are in different trust domains; only a USB flash
crosses them.

**"OTA rejected: version `<x>` is below the anti-rollback floor".** You are
installing an older build. Tick **Allow downgrade** at the confirm step, see
[Going backwards](#going-backwards-the-rollback-floor).

**"Node came back on `<version>`; the update did not stick."** The node
rebooted and is still running the old firmware. Nothing is broken: the node
is up on the image it had. Try the install again.

**"Node did not come back."** The web client waited 45 seconds after the
reboot and got no answer. First give it another minute and reconnect
manually: a slow WiFi rejoin looks exactly like this. If it stays dark, the
node needs a USB recovery:

- Easiest, no toolchain: connect the node over USB and reflash it with the
  [web flasher](https://bramblemesh.org/web-flasher/).
- From a source checkout, use the flash script, which applies the right
  board profile and build directory:

  ```bash
  bash scripts/flash.sh local heltec-v3 flash /dev/ttyUSB0
  ```

  Substitute your board (`heltec-v3`, `heltec-v4`, `tdeck-plus`,
  `bramble-pager`) and port. Do not reach for raw `esptool`: it skips the
  board defaults and has bricked devices here before
  ([troubleshooting.md](troubleshooting.md)).

Reflashing writes the firmware; on its own it does not wipe the node's stored
settings (`scripts/flash.sh` erases NVS only if you pass it `--erase-nvs`).

## How this was verified

The screen labels, button names, progress wording, and error text on this
page come from the web client source that renders them
(`webapp/src/pages/Config/DeviceManagementSection.tsx`,
`webapp/src/pages/Config/FirmwareUpdateCard.tsx`,
`webapp/src/pages/Config/otaFlow.ts`, `webapp/src/lib/otaIndex.ts`) and the
firmware that produces the failure messages (`components/ota/ota.c`,
`components/ota/ota_rollback.c`). The quoted strings were then confirmed by
rendering the firmware-update card in the webapp's own test environment
(vitest and jsdom) against a stubbed release index and a mocked node, walking
the confirm, progress, reboot, success, downgrade, index-failure, and
install-failure states.

What that does not cover: no real image was downloaded to real hardware for
this page, and the published-artifact statements above are a snapshot of
`https://bramblemesh.org/ota/index.json` on the date at the top.
