/**
 * Test for mock server bramble.sendBroadcast RPC method.
 * Validates that the mock node handles broadcast sends correctly.
 */

import { describe, it, expect, beforeAll, afterAll } from 'vitest';
import WebSocket from 'ws';
import { spawn } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { dirname, resolve } from 'node:path';

const MOCK_SERVER_URL = process.env.MOCK_SERVER_URL || 'ws://localhost:3005';
const __dirname = dirname(fileURLToPath(import.meta.url));
const serverPath = resolve(__dirname, '../../mock/server.mjs');

function sendRpc(ws, method, params = {}) {
  const id = Date.now();
  return new Promise((resolve, reject) => {
    const timeout = setTimeout(() => reject(new Error('RPC timeout')), 5000);

    const handler = (data) => {
      const msg = JSON.parse(data.toString());
      if (msg.id === id) {
        clearTimeout(timeout);
        ws.off('message', handler);
        if (msg.error) {
          reject(new Error(msg.error.message));
        } else {
          resolve(msg.result);
        }
      }
    };

    ws.on('message', handler);
    ws.send(JSON.stringify({ jsonrpc: '2.0', id, method, params }));
  });
}

function collectNotifications(ws, method, count, timeoutMs = 8000, filter = null) {
  return new Promise((resolve) => {
    const collected = [];
    const timeout = setTimeout(() => resolve(collected), timeoutMs);

    const handler = (data) => {
      const msg = JSON.parse(data.toString());
      if (msg.method === method && (!filter || filter(msg.params))) {
        collected.push(msg.params);
        if (collected.length >= count) {
          clearTimeout(timeout);
          ws.off('message', handler);
          resolve(collected);
        }
      }
    };

    ws.on('message', handler);
  });
}

describe('Mock Server bramble.sendBroadcast', () => {
  let ws;
  let mockServer;

  beforeAll(async () => {
    if (!process.env.MOCK_SERVER_URL) {
      mockServer = spawn(process.execPath, [serverPath], {
        stdio: 'ignore',
        env: { ...process.env, PORT: '3005' },
      });

      await new Promise((resolve, reject) => {
        const timeout = setTimeout(() => reject(new Error('Mock server startup timeout')), 5000);
        const tryConnect = () => {
          const probe = new WebSocket(MOCK_SERVER_URL);
          probe.once('open', () => {
            clearTimeout(timeout);
            probe.close();
            resolve();
          });
          probe.once('error', () => setTimeout(tryConnect, 100));
        };
        tryConnect();
      });
    }

    ws = new WebSocket(MOCK_SERVER_URL);
    await new Promise((resolve, reject) => {
      ws.on('open', resolve);
      ws.on('error', reject);
    });
  });

  afterAll(() => {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.close();
    }
    if (mockServer && !mockServer.killed) {
      mockServer.kill('SIGTERM');
    }
  });

  it('should return packetId and broadcastId', async () => {
    const result = await sendRpc(ws, 'bramble.sendBroadcast', { text: 'Test broadcast message' });
    
    expect(result).toBeDefined();
    expect(typeof result.packetId).toBe('number');
    expect(typeof result.broadcastId).toBe('string');
    expect(result.broadcastId).toMatch(/^bcast-/);
  });

  it('should emit onBroadcastDelivery notifications', async () => {
    // Send broadcast first so we have the broadcastId to filter on.
    // Delivery notifications have 500ms+ delays, so starting collection after
    // the RPC returns is safe and avoids collecting stale notifications from
    // other tests that share the same WebSocket connection.
    const result = await sendRpc(ws, 'bramble.sendBroadcast', { text: 'Hello mesh!' });

    // Collect only notifications for this specific broadcast
    const notifications = await collectNotifications(
      ws, 'bramble.onBroadcastDelivery', 2, 6000,
      (params) => params.broadcastId === result.broadcastId,
    );

    expect(notifications.length).toBeGreaterThan(0);

    // Verify notification structure
    const firstNotification = notifications[0];
    expect(firstNotification.broadcastId).toBe(result.broadcastId);
    expect(typeof firstNotification.from).toBe('number');
    expect(['delivered', 'failed']).toContain(firstNotification.status);
    expect(typeof firstNotification.hopCount).toBe('number');
    expect(typeof firstNotification.deliveredAtMs).toBe('number');
  });

  it('should NOT return Method not found error', async () => {
    // This is the key test — verifying the fix works
    const result = await sendRpc(ws, 'bramble.sendBroadcast', { text: 'Verify no method not found error' });
    
    // If we get here without error, the method exists
    expect(result).toBeDefined();
    expect(result.packetId).toBeDefined();
  });
});
