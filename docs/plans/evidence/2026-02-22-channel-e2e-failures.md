# 2026-02-22 Task 1 Evidence: Channel E2E Remaining Failures (.21/.64)

## Scope
Executed **Task 1** from `docs/plans/2026-02-22-channel-e2e-remaining-failures.md` with required hard gate first (web client rebuild/redeploy), then captured failing baseline on nodes `192.168.1.21` and `192.168.1.64` using web client `https://192.168.6.34:3443`.

## HARD GATE: Rebuild + redeploy web client docker stack

### Commands run
```bash
cd /home/justin/src/bramble/webapp && npm run build
cd /home/justin/src/bramble/webapp && docker compose -f docker-compose.yml build bramble-webapp
cd /home/justin/src/bramble/webapp && docker compose -f docker-compose.yml up -d bramble-webapp caddy
cd /home/justin/src/bramble/webapp && docker compose -f docker-compose.yml ps
```

### Outcomes
- Build succeeded.
- Docker image rebuilt (`webapp-bramble-webapp`).
- `bramble-webapp` container recreated and started.
- `caddy` running and serving `:3443`.

## Served bundle hash evidence (post-redeploy)

### Commands run
```bash
cd /home/justin/src/bramble/webapp && ls -l dist/assets
cd /home/justin/src/bramble/webapp && sha256sum dist/assets/index-CNGQnQa4.js dist/assets/index-T2TtSzvR.css dist/index.html
curl -k -s https://192.168.6.34:3443/ > /tmp/bramble-served-index.html
grep -oE 'assets/index-[^" ]+' /tmp/bramble-served-index.html | sort -u
sha256sum /tmp/bramble-served-index.html
head -n 20 /tmp/bramble-served-index.html
```

### Outcomes
- Dist assets:
  - `dist/assets/index-CNGQnQa4.js`
  - `dist/assets/index-T2TtSzvR.css`
- SHA256:
  - `index-CNGQnQa4.js`: `728c59e056637499040bc95ec5f971459d78641870893a99cf06819e12f5af8c`
  - `index-T2TtSzvR.css`: `8b41cb5c7c6a46513755049b842a93ece555d380742adfb36de5ae16d1e2c917`
  - `dist/index.html`: `0b884a0bf51d602080996dd49a5e8f1bf0af9d6b9c7834d91ac8786898f0a60c`
  - Served `/` html: `0b884a0bf51d602080996dd49a5e8f1bf0af9d6b9c7834d91ac8786898f0a60c`
- Served HTML references:
  - `./assets/index-CNGQnQa4.js`
  - `./assets/index-T2TtSzvR.css`

## Task 1 baseline reproduction (failing)

> Note: plan command `node /home/justin/.openclaw/workspace/tmp-e2e/repro-channel-failures.js` was attempted first and failed with `MODULE_NOT_FOUND` (path does not exist). Baseline was then captured via deterministic Playwright CLI runs with saved text/screenshot artifacts below.

### Commands run
```bash
node /home/justin/.openclaw/workspace/tmp-e2e/repro-channel-failures.js
```
Result:
- `Error: Cannot find module '/home/justin/.openclaw/workspace/tmp-e2e/repro-channel-failures.js'`

Then executed Playwright automation from `webapp/` to capture equivalent flow against `.21/.64`.

### Observed failures

1. **Create channel with explicit name + PSK fails metadata expectation**
   - Action on `.21`: create with `name=task1-psk`, `psk=hunter2`.
   - Observed: new channel created as `channel_5` (auto name), not `task1-psk`.
   - Evidence flag: `node21_create_has_task1_name=false`.

2. **Cross-node channel visibility/delivery mismatch**
   - Action: send `"task1 msg from .21"` in `channel_4` on `.21`; connect `.64` and open `channel_4`.
   - Observed: `.64` does **not** show `.21` message.
   - Evidence flag: `node64_sees_msg_from21=false`.

3. **Leave action behavior failure**
   - Action on `.64` in `channel_4`: open detail panel, click **Leave Channel**.
   - Observed: still in `channel_4` view and channel remains present.
   - Evidence: `after_leave_has_channel4=true, in_channel4_view=true`.

4. **Lock visibility failure (PSK indicator not visible)**
   - After name+PSK create attempt, detail/config UI shows no lock/PSK visual indicator for created channel context.
   - Evidence flag: `node21_lock_icon_present_in_detail=false`.

## Artifact paths

### Text dumps
- `/home/justin/src/bramble/tmp-e2e/task1-step1-node21-config.txt`
- `/home/justin/src/bramble/tmp-e2e/task1-step2-node21-chat-after-send.txt`
- `/home/justin/src/bramble/tmp-e2e/task1-step3-node21-detailpanel.txt`
- `/home/justin/src/bramble/tmp-e2e/task1-step4-node64-chat-initial.txt`
- `/home/justin/src/bramble/tmp-e2e/task1-step5-node64-channel4-before-send.txt`
- `/home/justin/src/bramble/tmp-e2e/task1-step6-node64-after-send.txt`
- `/home/justin/src/bramble/tmp-e2e/task1-step7-node64-detail-before-leave.txt`
- `/home/justin/src/bramble/tmp-e2e/task1-step8-node64-after-leave.txt`
- `/home/justin/src/bramble/tmp-e2e/task1-observations.log`

### Screenshots
- `/home/justin/src/bramble/tmp-e2e/task1-step1-node21-config.png`
- `/home/justin/src/bramble/tmp-e2e/task1-step2-node21-chat-after-send.png`
- `/home/justin/src/bramble/tmp-e2e/task1-step3-node21-detailpanel.png`
- `/home/justin/src/bramble/tmp-e2e/task1-step4-node64-chat-initial.png`
- `/home/justin/src/bramble/tmp-e2e/task1-step5-node64-channel4-before-send.png`
- `/home/justin/src/bramble/tmp-e2e/task1-step6-node64-after-send.png`
- `/home/justin/src/bramble/tmp-e2e/task1-step7-node64-detail-before-leave.png`
- `/home/justin/src/bramble/tmp-e2e/task1-step8-node64-after-leave.png`

## Baseline verdict
Task 1 failing baseline reproduced and captured for:
- name+PSK create metadata
- cross-node visibility/delivery
- leave action behavior
- lock visibility
