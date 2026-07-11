# Bramble emulator run targets.
#
# Every target here is a thin wrapper over the canonical commands documented
# in emulator/README.md and emulator/node/README.md, and the scripts under
# emulator/scripts/ and emulator/ci/. No build/run logic lives only here.

SHELL := /usr/bin/env bash

EMU_DIR   := $(abspath .)
REPO_ROOT := $(abspath ..)
NODE_DIR  := $(EMU_DIR)/node
NODE_BIN  := $(NODE_DIR)/build/bramble-node.elf
GOSIM_DIR := $(REPO_ROOT)/simulator/gosim
GOSIM_BIN := $(GOSIM_DIR)/bramble-gosim
UI_DIR    := $(REPO_ROOT)/simulator/ui
SCEN_DIR  := $(REPO_ROOT)/simulator/scenarios

PORT ?= 3000

.PHONY: check node broker ui run serve headless e2e clean

## check: verify the toolchain (idf.py + linux target, go, node/npm, jq; docker optional).
check:
	@bash "$(EMU_DIR)/scripts/check_prereqs.sh"

## node: build the linux firmware node binary.
node: check
	@bash "$(EMU_DIR)/scripts/build_node.sh"

## broker: build gosim.
broker: check
	@echo "==> building gosim broker"
	cd "$(GOSIM_DIR)" && go build -o bramble-gosim .
	@echo "broker binary: $(GOSIM_BIN)"

## ui: build the React UI.
ui: check
	@echo "==> building the React UI"
	cd "$(UI_DIR)" && npm ci && npm run build
	@echo "UI bundle: $(UI_DIR)/dist"

## run (alias: serve): check + build all, launch the live stack (broker +
## firmware nodes + UI), bound to 0.0.0.0 for review from another machine.
## Open the URL it prints and load the "emu-channel-delivery" scenario for a
## live provisioned 3-pager mesh, or "emulator-3-pagers" for an unprovisioned
## attach/persistence smoke.
run: node broker ui
	@echo "==> launching bramble-gosim: broker + UI, port $(PORT)"
	@echo "    gosim binds :$(PORT), i.e. 0.0.0.0:$(PORT) (verified: Go's"
	@echo "    net/http wildcard-binds an unqualified \":port\" address; no"
	@echo "    gosim flag or code change was needed for this)."
	@ip="$$(hostname -I 2>/dev/null | awk '{print $$1}')"; \
	echo "    open: http://localhost:$(PORT)/  (this host)"; \
	if [ -n "$$ip" ]; then echo "    or:   http://$$ip:$(PORT)/  (LAN / another machine)"; fi
	"$(GOSIM_BIN)" --ui "$(UI_DIR)/dist" --scenarios "$(SCEN_DIR)" --port "$(PORT)"

serve: run

## headless: run the CI scenario suite locally.
headless: node broker
	@echo "==> running the CI scenario suite"
	bash "$(EMU_DIR)/ci/run_scenarios.sh"

## e2e: browser-level acceptance suite (Playwright, headless chromium) against
## the full live stack -- gosim + real firmware node processes + the built UI.
## Proves the DISPLAY is correct and basic buttons/reset/messaging work
## through what a human actually sees, not just headless framebuffer asserts
## (that's `make headless`). See emulator/e2e/README.md.
e2e: node broker ui
	@cd "$(UI_DIR)" && npm install
	@bash "$(EMU_DIR)/e2e/run_e2e.sh"

## clean: remove build artifacts.
clean:
	@echo "==> cleaning build artifacts"
	rm -rf "$(NODE_DIR)/build" "$(NODE_DIR)/sdkconfig" "$(NODE_DIR)/sdkconfig.old" \
	       "$(NODE_DIR)/managed_components" "$(NODE_DIR)/dependencies.lock"
	rm -f "$(GOSIM_BIN)"
	rm -rf "$(UI_DIR)/dist"
	rm -rf "$(EMU_DIR)/e2e/.run" "$(EMU_DIR)/e2e/node_modules"
