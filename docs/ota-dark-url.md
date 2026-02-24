# OTA Dark URL

OTA artifacts are served from the existing Bramble site stack under an unlinked path:

- Base: `/ota/`
- Index: `/ota/index.json`

Policy:
- Do not link `/ota/` from public pages.
- Access by direct URL only.
- `index.json` is no-store; binaries are long-cache immutable.
