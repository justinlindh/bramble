#!/usr/bin/env bash
set -u

strict=0
changed=0
for arg in "$@"; do
  case "$arg" in
    --strict) strict=1 ;;
    --changed) changed=1 ;;
  esac
done

if command -v markdownlint-cli2 >/dev/null 2>&1; then
  lint_cmd=(markdownlint-cli2)
elif command -v npx >/dev/null 2>&1; then
  lint_cmd=(npx --yes markdownlint-cli2)
else
  if (( strict )); then
    echo "[markdownlint] FAIL: markdownlint-cli2 (or npx) not found."
    exit 1
  fi
  echo "[markdownlint] SKIP: markdownlint-cli2 (or npx) not found."
  exit 0
fi

patterns=()
if (( changed )); then
  if git rev-parse --verify HEAD~1 >/dev/null 2>&1; then
    mapfile -t files < <(git diff --name-only --diff-filter=ACMR HEAD~1..HEAD | grep -E '\\.md$' || true)
  else
    mapfile -t files < <(git ls-files '*.md')
  fi
  if [[ ${#files[@]} -eq 0 ]]; then
    echo "[markdownlint] PASS: no markdown files in changed scope."
    exit 0
  fi
  patterns=("${files[@]}")
else
  patterns=('**/*.md' '#node_modules')
fi

tmp_out="$(mktemp)"
trap 'rm -f "$tmp_out"' EXIT

if ! "${lint_cmd[@]}" "${patterns[@]}" >"$tmp_out" 2>&1; then
  if (( strict )); then
    echo "[markdownlint] FAIL: findings detected (showing up to 250 lines)."
    sed -n '1,250p' "$tmp_out"
    exit 1
  fi
  echo "[markdownlint] ADVISORY: findings detected (showing up to 250 lines)."
  sed -n '1,250p' "$tmp_out"
  echo "[markdownlint] Note: advisory mode, returning success."
  exit 0
fi

echo "[markdownlint] PASS: markdown files satisfy baseline rules."
exit 0
