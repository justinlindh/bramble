#!/usr/bin/env bash
set -u

if command -v markdownlint-cli2 >/dev/null 2>&1; then
  lint_cmd=(markdownlint-cli2)
elif command -v npx >/dev/null 2>&1; then
  lint_cmd=(npx --yes markdownlint-cli2)
else
  echo "[markdownlint] SKIP: markdownlint-cli2 (or npx) not found. Install markdownlint-cli2 to enable this advisory check."
  exit 0
fi

tmp_out="$(mktemp)"
trap 'rm -f "$tmp_out"' EXIT

if ! "${lint_cmd[@]}" '**/*.md' '#node_modules' >"$tmp_out" 2>&1; then
  echo "[markdownlint] ADVISORY: findings detected (showing up to 250 lines)."
  sed -n '1,250p' "$tmp_out"
  echo "[markdownlint] Note: advisory mode, returning success."
  exit 0
fi

echo "[markdownlint] PASS: markdown files satisfy baseline rules."
exit 0
