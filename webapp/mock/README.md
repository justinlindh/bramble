# bramble-mock-node

WebSocket mock node server for Bramble webapp development (`server.mjs`).

Run `npm install` in this directory before `npm start`; `node_modules/` is not
committed.

## Provisioning state

The mock boots **unprovisioned**, exactly like real firmware: it holds no
network key and reports the all-zero fingerprint sentinel. That makes the
UNPROVISIONED banner and the whole found/join flow in Config -> Network Key
reachable without hardware. `bramble.generateNetworkKey` mints a key and
provisions the mock atomically; `bramble.setNetworkKey` joins it to a key you
paste. State is per-process, so restarting the server gives you a fresh
unprovisioned node, which is how the two-node founder/joiner sequence in
[the provisioning screenshots](../scripts/capture-provisioning-shots.mjs) is
captured.
