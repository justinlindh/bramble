# Unified Runtime (Task 4)

## Smoke script

`./scripts/unified-runtime-smoke.sh` checks:
- `/`
- `/api/healthz`
- `/api/mode`

## Build and run

```bash
docker build -t bramble-web-client-unified:dev .
docker run --rm -p 8085:8085 -e MODE=hosted bramble-web-client-unified:dev
```

## Endpoint verification output

```text
==> GET http://127.0.0.1:8085/
HTTP/1.1 200 OK
Content-Type: text/html; charset=utf-8
...

<!DOCTYPE html>
<html lang="en">
...
</html>

==> GET http://127.0.0.1:8085/api/healthz
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
...

{"ok":true}

==> GET http://127.0.0.1:8085/api/mode
HTTP/1.1 200 OK
Content-Type: application/json; charset=utf-8
...

{"mode":"hosted"}
```

## Notes

- Container now serves both static Vite assets and API routes from `server/unified-server.mjs`.
- Runtime listens on port `8085`.
