#!/usr/bin/env bash
#
# build_node_heltec.sh: build the linux firmware node binary for the Virtual
# Heltec (SSD1306 128x64 OLED) profile, alongside the default Virtual Pager
# build. It is the OLED analogue of build_node.sh: same canonical ESP-IDF flow,
# but a separate build dir (build-heltec/) and sdkconfig so both profiles can
# coexist in one checkout.
#
#   source $IDF_PATH/export.sh
#   idf.py -B build-heltec -D SDKCONFIG=sdkconfig.heltec \
#          -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.heltec \
#          --preview set-target linux   (first build only)
#   idf.py -B build-heltec build
#
# ESP-IDF location: set IDF_PATH, or it defaults to ~/src/esp-idf.
# Output: emulator/node/build-heltec/bramble-node.elf

set -eu

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # emulator/
IDF_PATH="${IDF_PATH:-$HOME/src/esp-idf}"

echo "==> building linux firmware node, Heltec OLED profile (emulator/node -> build-heltec)"

# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null

cd "$HERE/node"

if [ ! -f sdkconfig.heltec ]; then
    idf.py -B build-heltec -D SDKCONFIG=sdkconfig.heltec \
        -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.heltec \
        --preview set-target linux
fi

idf.py -B build-heltec -D SDKCONFIG=sdkconfig.heltec \
    -D SDKCONFIG_DEFAULTS=sdkconfig.defaults.heltec build

echo "node binary: $HERE/node/build-heltec/bramble-node.elf"
