#!/usr/bin/env bash
# Attach files to an existing GitHub release, idempotently.
#
# Resolves the release by tag, fetches its asset list once, deletes any asset
# that would collide with an upload name, then uploads each file. Uses curl +
# python3 against the REST API rather than the gh CLI: the release runner is
# not guaranteed to ship gh, but it already has curl and the python3 that
# ESP-IDF needs.
#
# Usage: attach-release-assets.sh <tag> <file>...
# Env:   GH_TOKEN  token with contents:write (required)
#        REPO      owner/name (defaults to GITHUB_REPOSITORY)
set -euo pipefail

TAG="${1:?usage: attach-release-assets.sh <tag> <file>...}"
shift
[[ $# -ge 1 ]] || { echo "attach-release-assets: no files given" >&2; exit 2; }
: "${GH_TOKEN:?GH_TOKEN is required}"
REPO="${REPO:-${GITHUB_REPOSITORY:?REPO or GITHUB_REPOSITORY is required}}"

api="https://api.github.com"
auth=(-H "Authorization: Bearer $GH_TOKEN" -H "Accept: application/vnd.github+json")

release_id="$(curl -fsSL "${auth[@]}" "$api/repos/$REPO/releases/tags/$TAG" \
  | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')"
echo "Release id for $TAG: $release_id"

# One fetch of the existing assets; emits "id name" per line. per_page=100 is
# plenty for this repo's handful of factory images per release.
existing_assets="$(curl -fsSL "${auth[@]}" \
  "$api/repos/$REPO/releases/$release_id/assets?per_page=100" \
  | python3 -c 'import json,sys; [print(a["id"], a["name"]) for a in json.load(sys.stdin)]')"

for f in "$@"; do
  name="$(basename "$f")"
  while read -r asset_id asset_name; do
    if [[ "$asset_name" == "$name" ]]; then
      echo "Replacing existing asset $name (id $asset_id)"
      curl -fsSL -X DELETE "${auth[@]}" "$api/repos/$REPO/releases/assets/$asset_id"
    fi
  done <<< "$existing_assets"
  curl -fsSL "${auth[@]}" \
    -H "Content-Type: application/octet-stream" \
    --data-binary @"$f" \
    "https://uploads.github.com/repos/$REPO/releases/$release_id/assets?name=$name" \
    > /dev/null
  echo "Uploaded $name"
done
