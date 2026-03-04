# App domain release E2E evidence (Task 8 retry)

Date: 2026-02-24/25 PST  
Operator: OpenClaw subagent (retry run with strict network timeouts)

## Verification checklist (adapted to current state)

- [ ] main merge triggers publish
- [ ] image visible in registry
- [ ] deploy job runs
- [x] `app.bramblemesh.org` serves app health endpoint
- [ ] `bramblemesh.org` unaffected

## Constraints followed

- All registry/domain curls were executed on GPU host `192.168.1.199` over SSH.
- Every curl used explicit timeout: `--max-time 12`.
- No uncapped local-host curl was used against registry.

## Commands + captured outputs

### 1) Registry `/v2` status

Command:
```bash
ssh 192.168.1.199 "curl --max-time 12 -sS -o /tmp/registry.body -D /tmp/registry.headers -w 'HTTP_CODE=%{http_code}\n' https://registry.idiotica.org/v2/"
```

Observed:
```text
HTTP_CODE=401
HTTP/2 401
server: openresty
docker-distribution-api-version: registry/2.0
www-authenticate: Bearer realm="https://registry.idiotica.org/v2/token",service="container_registry",scope="*"
```

Result: Registry endpoint is reachable and responding with expected auth challenge.

### 2) App `8085` local health (on GPU host)

Command:
```bash
ssh 192.168.1.199 "curl --max-time 12 -sS -o /tmp/app8085.body -D /tmp/app8085.headers -w 'HTTP_CODE=%{http_code}\n' http://127.0.0.1:8085/healthz"
```

Observed:
```text
HTTP_CODE=200
HTTP/1.1 200 OK
Server: nginx/1.29.5
Content-Type: text/plain
```

Result: Local app container endpoint on port 8085 is healthy.

### 3) App domain `/healthz` status

Command:
```bash
ssh 192.168.1.199 "curl --max-time 12 -sS -o /tmp/appdomain.body -D /tmp/appdomain.headers -w 'HTTP_CODE=%{http_code}\n' https://app.bramblemesh.org/healthz"
```

Observed:
```text
HTTP_CODE=200
HTTP/2 200
x-served-by: app.bramblemesh.org
server: cloudflare
```

Result: Public app domain health endpoint is healthy.

### 4) Root site unaffected check (`bramblemesh.org`)

Command:
```bash
ssh 192.168.1.199 "curl --max-time 12 -sS -o /tmp/root.body -D /tmp/root.headers -w 'HTTP_CODE=%{http_code}\n' https://bramblemesh.org/"
```

Observed:
```text
HTTP_CODE=502
HTTP/2 502
server: cloudflare
```

Result: **FAIL** for unaffected-root-site criterion (currently returning 502).

### 5) Running containers/services reality

Command:
```bash
ssh 192.168.1.199 'docker ps --format "table {{.Names}}\t{{.Image}}\t{{.Status}}\t{{.Ports}}" | grep -E "NAMES|bramblemesh-site|bramble-web-client|bramblemesh-coming-soon|gitea|nginx|caddy"'
```

Observed:
```text
bramble-web-client        nginx:alpine   Up ...   0.0.0.0:8085->80/tcp
bramblemesh               nginx:alpine   Restarting (1) ...
gitea                     gitea/gitea:latest   Up ...
nginx-proxy               nginx:alpine   Up ...   0.0.0.0:80->80/tcp
caddy-internal            caddy:latest   Up ...   0.0.0.0:443->443/tcp
```

Additional `docker compose ps --format json` confirmed service reality in `/home/justin/src/dockers`:
- `bramble-web-client` is running.
- `bramblemesh` service is in restart loop.
- No running `bramblemesh-site` container was observed.

## Summary

- App-domain path is healthy (`app.bramblemesh.org/healthz` and local `:8085/healthz` both 200).
- Registry endpoint is reachable/auth-protected (`/v2` => 401 expected).
- Root domain check currently fails (`bramblemesh.org` => 502), so full E2E release validation is **not yet passing**.
- Runtime split appears incomplete/inconsistent with target naming (`bramblemesh` restarting; no `bramblemesh-site` running).

## Remaining blockers for full Task 8 pass

1. Restore root site availability (`https://bramblemesh.org/` should return 200).  
2. Resolve `bramblemesh` restart loop or complete migration to `bramblemesh-site` service.  
3. Capture/pin CI publish + deploy run IDs and image tag evidence (`main`, `sha-*`, semver) once runtime is stable.
