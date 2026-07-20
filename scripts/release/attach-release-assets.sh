#!/usr/bin/env bash
# Attach files to a GitHub release, then publish it.
#
# GitHub releases are immutable once published: the upload API rejects assets
# on a published release with HTTP 422 ("Cannot upload assets to an immutable
# release"). The release pipeline therefore creates firmware and webapp
# releases as DRAFTS (draftRelease: true in the .releaserc.<comp>.cjs github
# plugin), builds the assets, and hands them to this script, which uploads them
# to the still-mutable draft and then flips the draft to published. Publishing
# last means the release goes public already carrying its assets.
#
# Resolves the release by tag INCLUDING drafts (the /releases/tags/{tag}
# endpoint hides drafts, so this lists releases and matches tag_name), deletes
# any asset that would collide with an upload name, uploads each file, then
# publishes if the release is still a draft. Uses curl + python3 against the
# REST API rather than the gh CLI: the release runner is not guaranteed to ship
# gh, but it already has curl and the python3 that ESP-IDF needs.
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

# Find the release by tag, including drafts. Drafts are absent from
# /releases/tags/{tag}, so page through the release list and match tag_name.
# A newly created draft is the newest release (page 1); paginating keeps an
# older draft (a webapp_installers_tag rebuild) findable too. The listing is
# briefly eventually-consistent after a draft is created, so retry a few times:
# the real pipeline builds assets for minutes between create and attach and
# never hits this, but the retry keeps a cold list cache from flaking the step.
find_release() {
  local page=1 batch match count
  while :; do
    # Explicit exit check: this runs inside an `if` condition, where set -e is
    # ignored, so a curl failure (rate limit, transient 5xx) would otherwise
    # leave $batch empty, crash the python parsers on empty stdin, and spin the
    # loop until the step's timeout with no diagnostic. Returning 1 instead
    # counts the failure as a missed attempt and surfaces it through the caller's
    # 6-attempt retry, then the final "no release found" error.
    batch="$(curl -fsSL "${auth[@]}" "$api/repos/$REPO/releases?per_page=100&page=$page")" || return 1
    match="$(printf '%s' "$batch" | python3 -c '
import json, sys
tag = sys.argv[1]
for r in json.load(sys.stdin):
    if r.get("tag_name") == tag:
        print(json.dumps(r)); break
' "$TAG")"
    if [[ -n "$match" ]]; then printf '%s' "$match"; return 0; fi
    # Stop when the page came back short (last page).
    count="$(printf '%s' "$batch" | python3 -c 'import json,sys; print(len(json.load(sys.stdin)))')"
    [[ "$count" -lt 100 ]] && return 1
    page=$((page + 1))
  done
}

release_json=""
for attempt in 1 2 3 4 5 6; do
  if release_json="$(find_release)"; then break; fi
  echo "No release yet for tag $TAG (attempt $attempt); retrying" >&2
  sleep 5
done

[[ -n "$release_json" ]] || { echo "attach-release-assets: no release found for tag $TAG" >&2; exit 1; }

read -r release_id is_draft < <(printf '%s' "$release_json" \
  | python3 -c 'import json,sys; r=json.load(sys.stdin); print(r["id"], str(r.get("draft", False)).lower())')
echo "Release for $TAG: id=$release_id draft=$is_draft"

# Fail fast on an already-published release. This script's whole reason to exist
# is to attach assets while the release is still a mutable draft and then publish
# it; a published release is immutable and the upload API rejects new assets with
# a bare HTTP 422. Surfacing that here as a clear message beats letting the 422
# fall out of the upload loop below. In the normal pipeline the release is always
# a fresh draft; this only trips a webapp_installers_tag rebuild dispatch aimed at
# a tag whose release already published, which is unrecoverable by re-attach.
if [[ "$is_draft" != "true" ]]; then
  echo "attach-release-assets: release $TAG is already published and immutable;" \
       "assets cannot be added to it. Cut a new version instead." >&2
  exit 1
fi

# Existing assets, emitted "id name" per line, so an upload replaces rather than
# collides. A fresh draft has none; a re-attach on a still-draft release reuses
# this to replace by name. per_page=100 is plenty for this repo's handful of
# assets per release.
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

# Publish the draft now that its assets are attached. Publishing locks the
# release immutable, which is why this is the last step. The already-published
# case exited above, so the release is always a draft here.
curl -fsSL "${auth[@]}" -X PATCH "$api/repos/$REPO/releases/$release_id" \
  -d '{"draft":false}' > /dev/null
echo "Published release $TAG"
