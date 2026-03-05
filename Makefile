SHELL := /usr/bin/env bash
.DEFAULT_GOAL := help

.PHONY: help ci ci-quality ci-firmware-quality ci-webapp-quality \
	ci-quality-host-tests ci-quality-shellcheck ci-quality-actionlint ci-quality-ruff ci-quality-clang-format ci-quality-cppcheck ci-quality-board-build \
	ci-fw-clang-format ci-fw-shellcheck ci-fw-actionlint \
	ci-web-lint ci-web-typecheck ci-web-unit ci-web-build ci-web-smoke

help:
	@echo "CI parity targets"
	@echo "  make ci                 # run all local CI parity checks"
	@echo "  make ci-quality         # parity for .gitea/workflows/quality.yml"
	@echo "  make ci-firmware-quality# parity for .gitea/workflows/firmware-quality.yml"
	@echo "  make ci-webapp-quality  # parity for .gitea/workflows/webapp-quality.yml"

ci: ci-quality ci-firmware-quality ci-webapp-quality

ci-quality: ci-quality-host-tests ci-quality-shellcheck ci-quality-actionlint ci-quality-ruff ci-quality-clang-format ci-quality-cppcheck ci-quality-board-build

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
		scripts/test-ota-ci-board-matrix.sh \
		scripts/traffic-capture.sh \
		scripts/validate-broadcast-telemetry.sh

ci-quality-actionlint:
	command -v actionlint >/dev/null
	actionlint -color -oneline -ignore 'shellcheck reported issue.*SC2317' -config-file .actionlint.yaml .gitea/workflows/quality.yml

ci-quality-ruff:
	command -v uvx >/dev/null
	uvx --from 'ruff==0.12.10' ruff check scripts --select E9,F63,F7,F82

ci-quality-clang-format:
	bash scripts/lint/run-clang-format-check.sh --strict --changed

ci-quality-cppcheck:
	command -v cppcheck >/dev/null
	cppcheck --enable=warning,performance,portability --std=c11 --quiet main components

ci-quality-board-build:
	# Run board build on GPU box to avoid local build-dir ownership/toolchain drift.
	ssh justin@192.0.2.199 'cd ~/src/bramble && git fetch origin && git checkout fix/ci-idf-prebake && git reset --hard origin/fix/ci-idf-prebake && bash scripts/flash.sh local heltec-v3 build'

ci-firmware-quality: ci-fw-clang-format ci-fw-shellcheck ci-fw-actionlint

ci-fw-clang-format:
	bash scripts/lint/run-clang-format-check.sh --strict --changed

ci-fw-shellcheck:
	bash scripts/lint/run-shellcheck.sh --strict

ci-fw-actionlint:
	if command -v actionlint >/dev/null; then \
		actionlint -color -oneline -ignore 'shellcheck reported issue.*SC2317' -config-file .actionlint.yaml .gitea/workflows/firmware-quality.yml; \
	else \
		go run github.com/rhysd/actionlint/cmd/actionlint@v1.7.7 -color -oneline -ignore 'shellcheck reported issue.*SC2317' -config-file .actionlint.yaml .gitea/workflows/firmware-quality.yml; \
	fi

ci-webapp-quality: ci-web-lint ci-web-typecheck ci-web-unit ci-web-build ci-web-smoke

ci-web-lint:
	npm run lint --prefix webapp

ci-web-typecheck:
	npm run typecheck --prefix webapp

ci-web-unit:
	npm run test:unit --prefix webapp

ci-web-build:
	npm run build --prefix webapp

ci-web-smoke:
	npm run test:e2e:smoke --prefix webapp
