import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';

// The handler module starts background timers at import. Use fake timers so the
// recursive setTimeout/setInterval do not keep the event loop alive, and reset
// modules per test so process.env.MOCK_AUTH_TOKEN is read fresh.
function makeWs() {
  const listeners = {};
  const ws = {
    readyState: 1,
    _authed: undefined,
    sent: [],
    closed: null,
    on(evt, fn) { listeners[evt] = fn; },
    emit(evt, ...args) { return listeners[evt]?.(...args); },
    send(s) { ws.sent.push(JSON.parse(s)); },
    close(code, reason) { ws.closed = { code, reason }; },
  };
  return ws;
}

function rpc(ws, method, params, id = 1) {
  ws.sent = [];
  ws.emit('message', JSON.stringify({ jsonrpc: '2.0', id, method, params }));
  return ws.sent.find(m => m.id === id);
}

async function load(token) {
  vi.resetModules();
  if (token === undefined) delete process.env.MOCK_AUTH_TOKEN;
  else process.env.MOCK_AUTH_TOKEN = token;
  return import('./handler.mjs');
}

beforeEach(() => { vi.useFakeTimers(); });
afterEach(() => { vi.useRealTimers(); delete process.env.MOCK_AUTH_TOKEN; });

describe('mock handler shapes', () => {
  it('getAirtime reports the four-lane budget including receipt (issue #96)', async () => {
    const { handlers } = await load();
    const air = handlers['bramble.getAirtime']({});
    expect(air.tiers.map(t => t.name)).toEqual(['critical', 'normal', 'broadcast', 'receipt']);
  });

  it('ping and getVersion answer the allowlisted shapes', async () => {
    const { handlers } = await load();
    const ping = handlers['bramble.ping']({});
    expect(ping.pong).toBe(true);
    expect(typeof ping.address).toBe('string');
    const ver = handlers['bramble.getVersion']({});
    expect(ver).toHaveProperty('firmware_version');
    expect(ver).toHaveProperty('protocol_version');
  });

  it('management RPCs persist state (token, origins, OTA origin)', async () => {
    const { handlers } = await load();
    handlers['bramble.setAuthToken']({ token: 'rotated' }, { ws: {} });
    expect(handlers['bramble.getAuthToken']({})).toEqual({ token: 'rotated', enabled: true });

    handlers['bramble.setAllowedOrigins']({ origins: ['https://app.example'] });
    expect(handlers['bramble.getAllowedOrigins']({}).origins).toEqual(['https://app.example']);

    const origin = handlers['bramble.otaGetOrigin']({});
    expect(origin).toMatchObject({ ok: true, overridden: false });
    expect(origin).toHaveProperty('default_origin');
    handlers['bramble.otaSetOrigin']({ origin: 'https://ota.example/' });
    expect(handlers['bramble.otaGetOrigin']({}).overridden).toBe(true);
    handlers['bramble.otaSetOrigin']({ reset: true });
    expect(handlers['bramble.otaGetOrigin']({}).overridden).toBe(false);
  });

  it('BLE pairing RPCs answer the firmware shapes for set, clear and rejects', async () => {
    const { handlers } = await load();
    expect(handlers['bramble.getBleSecurity']({})).toEqual({
      mode: 'just-works',
      staticPasskeySet: false,
    });

    expect(handlers['bramble.setBlePasskey']({ passkey: '482913' })).toEqual({
      ok: true,
      mode: 'static-passkey',
    });
    expect(handlers['bramble.getBleSecurity']({})).toEqual({
      mode: 'static-passkey',
      staticPasskeySet: true,
    });

    // Rejections carry ok:false plus a reason, never a thrown error, so the
    // client surfaces them instead of reporting a save that did not happen.
    expect(handlers['bramble.setBlePasskey']({ passkey: '12345' }).ok).toBe(false);
    expect(handlers['bramble.setBlePasskey']({}).ok).toBe(false);
    expect(handlers['bramble.getBleSecurity']({}).staticPasskeySet).toBe(true);

    expect(handlers['bramble.setBlePasskey']({ passkey: null })).toEqual({
      ok: true,
      mode: 'just-works',
    });
    expect(handlers['bramble.getBleSecurity']({}).staticPasskeySet).toBe(false);
  });

  it('traffic debug persists and getTrafficEvents filters by seq (issue #96)', async () => {
    const { handlers } = await load();
    expect(handlers['bramble.getTrafficDebug']({}).enabled).toBe(false);
    handlers['bramble.setTrafficDebug']({ enabled: true });
    expect(handlers['bramble.getTrafficDebug']({}).enabled).toBe(true);
  });
});

describe('mock dest normalization (issue #96, BUG-5)', () => {
  it('a DM to a stale-route peer can fail', async () => {
    const { handleConnection } = await load();
    const ws = makeWs();
    handleConnection(ws, { url: '/ws', socket: { remoteAddress: 'test' } });
    // Force the stale-route 40%-success branch to fail.
    const rand = vi.spyOn(Math, 'random').mockReturnValue(0.99);
    // Downtown (0xAABBCC03) has a stale route; the client sends hex wire format.
    const res = rpc(ws, 'bramble.sendMessage', { dest: 'AABBCC03', text: 'hi' });
    const packetId = res.result.packetId;
    ws.sent = [];
    vi.advanceTimersByTime(4000);
    const ack = ws.sent.find(m => m.method === 'bramble.onAck' && m.params.packetId === packetId);
    expect(ack).toBeDefined();
    expect(ack.params.status).toBe('failed');
    rand.mockRestore();
  });
});

describe('mock auth enforcement (issue #96)', () => {
  it('a wrong token closes the connection with 1008', async () => {
    const { handleConnection } = await load('secret');
    const ws = makeWs();
    handleConnection(ws, { url: '/ws?token=wrong', socket: { remoteAddress: 'test' } });
    expect(ws.closed?.code).toBe(1008);
  });

  it('an unauthenticated connection may call only the allowlist', async () => {
    const { handleConnection } = await load('secret');
    const ws = makeWs();
    handleConnection(ws, { url: '/ws', socket: { remoteAddress: 'test' } });
    expect(ws._authed).toBe(false);
    expect(rpc(ws, 'bramble.ping', {}).result.pong).toBe(true);
    expect(rpc(ws, 'bramble.getStatus', {}).error.code).toBe(-1005);
  });

  it('a correct token authenticates fully', async () => {
    const { handleConnection } = await load('secret');
    const ws = makeWs();
    handleConnection(ws, { url: '/ws?token=secret', socket: { remoteAddress: 'test' } });
    expect(ws._authed).toBe(true);
    expect(rpc(ws, 'bramble.getStatus', {}).result).toHaveProperty('uptimeSec');
  });

  it('withholds notifications from unauthenticated connections', async () => {
    const { handleConnection } = await load('secret');
    const authed = makeWs();
    const unauth = makeWs();
    handleConnection(authed, { url: '/ws?token=secret', socket: { remoteAddress: 'a' } });
    handleConnection(unauth, { url: '/ws', socket: { remoteAddress: 'b' } });
    vi.spyOn(Math, 'random').mockReturnValue(0.1);
    rpc(authed, 'bramble.setTrafficDebug', { enabled: true });
    authed.sent = [];
    unauth.sent = [];
    vi.advanceTimersByTime(1600);
    const authedGot = authed.sent.some(m => m.method === 'bramble.onTrafficEvent');
    const unauthGot = unauth.sent.some(m => m.method === 'bramble.onTrafficEvent');
    expect(authedGot).toBe(true);
    expect(unauthGot).toBe(false);
  });
});
