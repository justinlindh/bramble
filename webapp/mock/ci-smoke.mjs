#!/usr/bin/env node
// CI smoke check for the mock node server (issue: webapp/mock has its own
// package.json and package-lock.json, but nothing installed or ran against
// them, so a dependency bump could break the module outright and CI stayed
// green).
//
// Run this AFTER `npm ci` inside webapp/mock, and run it from THIS
// directory: importing './handler.mjs' from here resolves through
// webapp/mock/node_modules first, exercising the module against the exact
// lockfile a contributor or Dependabot would install, not the webapp root's
// hoisted (and possibly different-major-version) copy of the same package.
//
// #181 bumped @noble/hashes to 2.x in this lockfile, which dropped the
// extensionless subpath exports handler.mjs imported; the import itself
// threw. This check both imports the module and drives one real RPC
// round-trip through handleConnection, so a working import with a broken
// handler (or a broken ws re-export) would still fail loudly.

import { handleConnection } from './handler.mjs';

function makeFakeWs() {
  const listeners = {};
  const sent = [];
  return {
    readyState: 1,
    sent,
    on(event, fn) {
      listeners[event] = fn;
    },
    emit(event, ...args) {
      return listeners[event]?.(...args);
    },
    send(payload) {
      sent.push(JSON.parse(payload));
    },
    close() {},
  };
}

const ws = makeFakeWs();
handleConnection(ws, { url: '/', socket: { remoteAddress: '127.0.0.1' } });
ws.emit('message', JSON.stringify({ jsonrpc: '2.0', id: 1, method: 'bramble.ping', params: {} }));

const reply = ws.sent.find((m) => m.id === 1);
if (!reply || !reply.result || reply.result.pong !== true) {
  console.error('mock smoke check FAILED: unexpected bramble.ping reply', reply);
  process.exit(1);
}

console.log('mock smoke check ok: handler.mjs loaded under its own lockfile and answered bramble.ping');
process.exit(0);
