#!/usr/bin/env node
// CI smoke check for the mock node server (issue: webapp/mock has its own
// package.json and package-lock.json, but nothing installed or ran against
// them, so a dependency bump could break the module outright and CI stayed
// green).
//
// Run this AFTER `npm ci` inside webapp/mock, and run it from THIS
// directory: it starts the real server.mjs and connects a real WebSocket
// client, both resolving through webapp/mock/node_modules, so the exact
// lockfile a contributor or Dependabot would install is exercised rather
// than the webapp root's hoisted copies.
//
// Starting server.mjs (not just importing handler.mjs) is deliberate: it
// exercises BOTH of this package's runtime dependencies. #181 bumped
// @noble/hashes to 2.x, which dropped the extensionless subpath exports
// handler.mjs imported and threw on import; a ws bump that broke the same
// way would only surface through the server's WebSocketServer, which a
// handler-only import never touches. This drives server.mjs (ws server) +
// a ws client + handler.mjs (@noble/hashes) in one round trip, so a break
// in either dependency fails this check.

import { spawn } from 'node:child_process';
import { WebSocket } from 'ws';

const PORT = 33005;
const READY_LINE = `listening on ws://0.0.0.0:${PORT}`;
const OVERALL_TIMEOUT_MS = 15000;

const server = spawn('node', ['server.mjs'], {
  cwd: import.meta.dirname,
  env: { ...process.env, PORT: String(PORT) },
  stdio: ['ignore', 'pipe', 'pipe'],
});

let serverLog = '';
let ready = false;
let done = false;

function cleanup() {
  if (!server.killed) server.kill('SIGTERM');
}

function fail(msg, extra) {
  console.error(`mock smoke check FAILED: ${msg}`);
  if (extra) console.error(extra);
  cleanup();
  process.exit(1);
}

const overall = setTimeout(() => {
  if (!done) fail(`timed out after ${OVERALL_TIMEOUT_MS}ms`, serverLog);
}, OVERALL_TIMEOUT_MS);

server.stdout.on('data', (chunk) => {
  serverLog += chunk;
  if (!ready && serverLog.includes(READY_LINE)) {
    ready = true;
    connectAndPing();
  }
});
server.stderr.on('data', (chunk) => {
  serverLog += chunk;
});
server.on('exit', (code) => {
  if (!done) fail(`server.mjs exited early (code ${code}) before the smoke check completed`, serverLog);
});

function connectAndPing() {
  const ws = new WebSocket(`ws://127.0.0.1:${PORT}`);
  ws.on('open', () => {
    ws.send(JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'bramble.ping', params: {} }));
  });
  ws.on('message', (data) => {
    let reply;
    try {
      reply = JSON.parse(data.toString());
    } catch (err) {
      fail('reply was not valid JSON', String(err));
      return;
    }
    if (reply.id !== 1 || !reply.result || reply.result.pong !== true) {
      fail('unexpected bramble.ping reply', JSON.stringify(reply));
      return;
    }
    done = true;
    clearTimeout(overall);
    ws.close();
    cleanup();
    console.log('mock smoke check ok: server.mjs started under its own lockfile and answered bramble.ping over a real WebSocket');
    process.exit(0);
  });
  ws.on('error', (err) => fail(`WebSocket client error: ${err.message}`, serverLog));
}
