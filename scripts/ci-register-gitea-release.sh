#!/usr/bin/env bash
set -euo pipefail

: "${GITEA_TOKEN:?GITEA_TOKEN is required}"

REPO="${GITEA_REPO:-${GITHUB_REPOSITORY:-}}"
: "${REPO:?GITEA_REPO or GITHUB_REPOSITORY is required}"

TAG="${RELEASE_TAG:-${GITHUB_REF_NAME:-}}"
: "${TAG:?RELEASE_TAG or GITHUB_REF_NAME is required}"

NAME="${RELEASE_NAME:-$TAG}"
BODY_FILE="${RELEASE_BODY_FILE:-}"
BODY="${RELEASE_BODY:-}"
GITEA_URL="${GITEA_URL:-${GITHUB_SERVER_URL:-https://github.com}}"
API_BASE="${GITEA_URL%/}/api/v1/repos/${REPO}"

if [[ -n "$BODY_FILE" ]]; then
  BODY="$(cat "$BODY_FILE")"
fi

headers=(
  -H "Authorization: token ${GITEA_TOKEN}"
  -H "Content-Type: application/json"
)

release_json="$(mktemp)"
http_code="$(curl -sS -o "$release_json" -w '%{http_code}' "${headers[@]}" "$API_BASE/releases/tags/$TAG")"

if [[ "$http_code" == "200" ]]; then
  release_id="$(jq -r '.id' "$release_json")"
  curl -sS -X PATCH "${headers[@]}" \
    -d "$(jq -n --arg name "$NAME" --arg body "$BODY" '{name:$name, body:$body, draft:false, prerelease:false}')" \
    "$API_BASE/releases/$release_id" >/dev/null
  echo "Updated existing release for tag $TAG (id=$release_id)"
else
  create_json="$(mktemp)"
  curl -sS -o "$create_json" -X POST "${headers[@]}" \
    -d "$(jq -n --arg tag "$TAG" --arg name "$NAME" --arg body "$BODY" '{tag_name:$tag, name:$name, body:$body, draft:false, prerelease:false, target_commitish:"main"}')" \
    "$API_BASE/releases" >/dev/null
  release_id="$(jq -r '.id' "$create_json")"
  if [[ -z "$release_id" || "$release_id" == "null" ]]; then
    echo "failed to create release for $TAG"
    cat "$create_json"
    exit 1
  fi
  rm -f "$create_json"
  echo "Created release for tag $TAG (id=$release_id)"
fi

assets_json="$(mktemp)"
curl -sS "${headers[@]}" "$API_BASE/releases/$release_id/assets" > "$assets_json"

upload_asset() {
  local file="$1"
  local rel name encoded
  rel="${file#build-artifacts/firmware-ci/}"
  name="${rel//\//-}"
  encoded="$(jq -rn --arg v "$name" '$v|@uri')"

  existing_id="$(jq -r --arg name "$name" '.[] | select(.name == $name) | .id' "$assets_json" | head -n1)"
  if [[ -n "$existing_id" && "$existing_id" != "null" ]]; then
    curl -sS -X DELETE -H "Authorization: token ${GITEA_TOKEN}" \
      "$API_BASE/releases/$release_id/assets/$existing_id" >/dev/null
    echo "Replaced existing asset: $name"
  fi

  curl -sS -X POST \
    -H "Authorization: token ${GITEA_TOKEN}" \
    -H "Content-Type: application/octet-stream" \
    --data-binary @"$file" \
    "$API_BASE/releases/$release_id/assets?name=$encoded" >/dev/null

  echo "Uploaded asset: $name"
}

if [[ -n "${ASSET_GLOBS:-}" ]]; then
  while IFS= read -r glob; do
    [[ -z "$glob" ]] && continue
    while IFS= read -r file; do
      upload_asset "$file"
    done < <(compgen -G "$glob" || true)
  done <<< "$ASSET_GLOBS"
fi

release_url="${GITEA_URL%/}/${REPO}/releases/tag/${TAG}"
echo "Release URL: $release_url"
