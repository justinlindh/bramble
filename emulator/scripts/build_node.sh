#!/usr/bin/env bash
#
# build_node.sh: build the linux firmware node binary (the Makefile `node`
# target). Wraps the canonical build documented in emulator/node/README.md:
#
#   source $IDF_PATH/export.sh
#   idf.py --preview set-target linux   (first build only)
#   idf.py build
#
# ESP-IDF location: set IDF_PATH, or it defaults to ~/src/esp-idf.

set -eu

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"   # emulator/
IDF_PATH="${IDF_PATH:-$HOME/src/esp-idf}"

echo "==> building linux firmware node (emulator/node)"

# shellcheck disable=SC1091
source "$IDF_PATH/export.sh" >/dev/null

cd "$HERE/node"

if [ ! -f sdkconfig ]; then
    idf.py --preview set-target linux
fi

idf.py build

echo "node binary: $HERE/node/build/bramble-node.elf"
