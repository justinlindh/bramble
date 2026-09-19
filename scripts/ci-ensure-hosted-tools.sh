#!/usr/bin/env bash
# Provision a GitHub-hosted runner with the tools the self-hosted runner image
# bakes in (bramble#222). Fork pull requests run GitHub-hosted so fork code
# never reaches the self-hosted pool, and the hosted image has none of these.
#
# Every caller gates this on `runner.environment == 'github-hosted'`, so
# self-hosted runs never reach it and stay free of network installs. Each
# version comes from the repo's existing pin rather than being restated here,
# and the calling job's own assert step still runs afterwards and checks the
# result, so a drifted install fails the same way a drifted image does.
set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

profile="${1:?usage: ci-ensure-hosted-tools.sh <static-checks|commitlint|nrf|emulator|emulator-e2e>}"
tools="${RUNNER_TEMP:?RUNNER_TEMP is unset: this script only runs inside a GitHub Actions job}/hosted-tools"
mkdir -p "$tools/bin"

# The quoted value assigned to a shell variable in a script: pin_from FILE NAME.
pin_from() {
  local value
  value="$(sed -n "s/^$2=\"\\(.*\\)\"\$/\\1/p" "$1" | head -1)"
  if [[ -z "$value" ]]; then
    echo "::error::no $2 pin found in $1" >&2
    exit 1
  fi
  printf '%s' "$value"
}

# The ESP-IDF jobs run as root inside a job container with no sudo; the rest run
# as the hosted runner's unprivileged user.
apt_install() {
  local as_root=()
  [[ "$(id -u)" -eq 0 ]] || as_root=(sudo)
  "${as_root[@]}" apt-get update -qq
  "${as_root[@]}" apt-get install -y -qq --no-install-recommends "$@"
}

case "$profile" in
  static-checks)
    clang_format_version="$(tr -d '[:space:]' < .clang-format-version)"
    ruff_version="$(pin_from scripts/lint/run-ruff.sh RUFF_VERSION)"
    markdownlint_version="$(pin_from scripts/lint/run-markdownlint.sh MARKDOWNLINT_CLI2_VERSION)"
    # The Makefile's `go run` fallback is the repo's actionlint pin.
    actionlint_version="$(sed -n 's#.*rhysd/actionlint/cmd/actionlint@\(v[0-9.]*\).*#\1#p' Makefile | head -1)"
    [[ -n "$actionlint_version" ]] || { echo "::error::no actionlint pin found in Makefile" >&2; exit 1; }

    # pipx keeps each tool in its own venv and exposes only its entry point, so
    # nothing else on the runner's PATH changes.
    export PIPX_HOME="$tools/pipx" PIPX_BIN_DIR="$tools/bin"
    pipx install --quiet "clang-format==${clang_format_version}"
    pipx install --quiet "ruff==${ruff_version}"
    GOBIN="$tools/bin" go install "github.com/rhysd/actionlint/cmd/actionlint@${actionlint_version}"
    npm install --global --silent "markdownlint-cli2@${markdownlint_version}"
    apt_install cppcheck
    ;;
  commitlint)
    major="${2:?usage: ci-ensure-hosted-tools.sh commitlint <major>}"
    npm install --global --silent "@commitlint/cli@${major}" "@commitlint/config-conventional@${major}"
    ;;
  nrf)
    # The Ubuntu 24.04 distro toolchain is the one nrf/README.md names as
    # reproducing CI's byte counts; the job's assert checks it against
    # .arm-gcc-version.
    apt_install gcc-arm-none-eabi libnewlib-arm-none-eabi ninja-build ccache
    ;;
  emulator)
    # What emulator/Dockerfile adds to the same espressif/idf base for the linux
    # target and the gosim cgo build, plus jq for the scenario scripts.
    apt_install build-essential pkg-config libbsd-dev libssl-dev jq
    ;;
  emulator-e2e)
    # Runs after simulator/ui's `npm ci`, so the browser revision installed is
    # the one that lockfile's playwright resolves. run_e2e.sh asserts both
    # binaries under PLAYWRIGHT_BROWSERS_PATH.
    export PLAYWRIGHT_BROWSERS_PATH="$tools/ms-playwright"
    echo "PLAYWRIGHT_BROWSERS_PATH=$PLAYWRIGHT_BROWSERS_PATH" >> "${GITHUB_ENV:?GITHUB_ENV is unset}"
    (cd simulator/ui && npx playwright install --with-deps chromium)
    ;;
  *)
    echo "::error::unknown profile: $profile" >&2
    exit 1
    ;;
esac

# Ahead of the hosted image's own copies (its clang-format is a newer major).
echo "$tools/bin" >> "${GITHUB_PATH:?GITHUB_PATH is unset}"
