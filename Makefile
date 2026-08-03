SHELL := /usr/bin/env bash
.DEFAULT_GOAL := help

.PHONY: help setup-hooks ci check-fast ci-quality ci-firmware-quality ci-webapp-quality \
	ci-quality-host-tests ci-quality-shellcheck ci-quality-ruff ci-quality-cppcheck ci-quality-board-build \
	ci-fw-clang-format ci-fw-shellcheck ci-fw-actionlint \
	ci-web-typecheck ci-web-typecheck-electron ci-web-unit ci-web-build ci-web-smoke

setup-hooks:
	git config core.hooksPath githooks
	@echo "Git hooks configured (githooks/)"

# ── Packaging ────────────────────────────────────────────────────────────
# Desktop (Electron) and Android builds. The Android app lives in a sibling
# repo; point BRAMBLE_ANDROID_DIR at your checkout if it is somewhere else.
BRAMBLE_ANDROID_DIR ?= $(HOME)/src/bramble-android

.PHONY: package-linux package-win package-android package-all

package-linux: ## Electron AppImage + deb + pacman into webapp/release/
	cd webapp && npm run package:linux
	@ls -1 webapp/release/*.AppImage webapp/release/*.deb webapp/release/*.pacman 2>/dev/null || true

package-win: ## Electron Windows NSIS installer (needs wine for cross-build)
	cd webapp && WINEDLLOVERRIDES="mscoree,mshtml=" WINEDEBUG=-all npm run package:win

package-android: ## Sync webapp into the Android shell repo and build the APK
	cd webapp && npm run build
	bash $(BRAMBLE_ANDROID_DIR)/scripts/sync-webapp-assets.sh $(CURDIR)/webapp
	cd $(BRAMBLE_ANDROID_DIR) && \
		JAVA_HOME=$$(mise where java@temurin-17) \
		ANDROID_HOME=$$(mise where android-sdk) \
		./gradlew assembleDebug --no-daemon
	@echo "APK: $(BRAMBLE_ANDROID_DIR)/app/build/outputs/apk/debug/app-debug.apk"

package-all: package-linux package-android ## Everything installable (win excluded; see package-win)

.PHONY: flash-fleet

flash-fleet: ## Flash every connected bench node (auto-applies the V3 --encrypt rule)
	bash scripts/flash-fleet.sh build

help:
	@echo "Setup"
	@echo "  make setup-hooks        # install the pre-commit hooks (run once per clone)"
	@echo "                          # see CONTRIBUTING.md"
	@echo "CI parity targets"
	@echo "  make ci                 # run all local CI parity checks"
	@echo "  make ci-quality         # host tests, board build, broader script sweep"
	@echo "  make ci-firmware-quality# the strict lint gates CI runs in Static checks"
	@echo "  make ci-webapp-quality  # webapp typecheck, unit tests, build, smoke"
	@echo "  make check-fast         # webapp typecheck + unit tests (what the pre-commit hook runs)"
	@echo "Packaging targets"
	@echo "  make package-linux      # Electron AppImage + deb + pacman (webapp/release/)"
	@echo "  make package-android    # webapp -> android assets -> debug APK"
	@echo "  make package-win        # Electron Windows installer (wine)"
	@echo "  make package-all        # linux + android"

# Fast pre-commit gate: typecheck + unit tests only (no build/lint/smoke)
check-fast: ci-web-typecheck ci-web-typecheck-electron ci-web-unit

ci: ci-quality ci-firmware-quality ci-webapp-quality

ci-quality: ci-quality-host-tests ci-quality-shellcheck ci-quality-ruff ci-quality-cppcheck ci-quality-board-build

ci-quality-host-tests:
	bash test/run_all_tests.sh

ci-quality-shellcheck:
	command -v shellcheck >/dev/null
	shellcheck \
		scripts/ci-build-firmware.sh \
		scripts/ci-publish-ota.sh \
		scripts/flash-all.sh \
		scripts/publish-firmware-release.sh \
		scripts/sha256-artifacts.sh \
		scripts/traffic-capture.sh \
		scripts/validate-broadcast-telemetry.sh

ci-quality-ruff:
	command -v uvx >/dev/null
	uvx --from 'ruff==0.12.10' ruff check scripts --select E9,F63,F7,F82

ci-quality-cppcheck:
	command -v cppcheck >/dev/null
	cppcheck --enable=warning,performance,portability --std=c11 --quiet --error-exitcode=2 --suppress=normalCheckLevelMaxBranches main components

ci-quality-board-build:
	bash scripts/ci-ensure-idf.sh
	bash scripts/flash.sh local heltec-v3 build

ci-firmware-quality: ci-fw-clang-format ci-fw-shellcheck ci-fw-actionlint

# The strict clang-format sweep lives here only. firmware-quality.yml is the
# single workflow that runs it, so `make ci` runs it once, not once per
# aggregate.
ci-fw-clang-format:
	bash scripts/lint/run-clang-format-check.sh --strict

ci-fw-shellcheck:
	bash scripts/lint/run-shellcheck.sh --strict

# Mirrors the "Actionlint (every workflow)" step in firmware-quality.yml: the
# glob is the point, so a workflow added later is covered by construction.
ci-fw-actionlint:
	if command -v actionlint >/dev/null; then \
		actionlint -color -oneline -ignore 'shellcheck reported issue.*SC2317' -config-file .actionlint.yaml .github/workflows/*.yml; \
	else \
		go run github.com/rhysd/actionlint/cmd/actionlint@v1.7.7 -color -oneline -ignore 'shellcheck reported issue.*SC2317' -config-file .actionlint.yaml .github/workflows/*.yml; \
	fi

ci-webapp-quality: ci-web-typecheck ci-web-typecheck-electron ci-web-unit ci-web-build ci-web-smoke

ci-web-typecheck:
	npm run typecheck --prefix webapp

ci-web-typecheck-electron:
	npm run typecheck:electron --prefix webapp

ci-web-unit:
	npm run test:unit --prefix webapp

# vite directly, not `npm run build`: that script is
# `npm run typecheck && vite build` and ci-web-typecheck above already ran
# that exact tsc over this checkout. The emitted bundle is identical; only the
# redundant typecheck is dropped. This matches the Build step in
# webapp-quality.yml. `npm run build` keeps its typecheck for standalone local
# use, where nothing ran tsc beforehand.
ci-web-build:
	cd webapp && npx vite build

ci-web-smoke:
	npm run test:e2e:smoke --prefix webapp
