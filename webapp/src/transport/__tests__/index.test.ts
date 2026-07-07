import { describe, it, expect, vi, afterEach } from 'vitest';

// Wiring check for the Mock Node connect button bug: in embedded shells
// (Android WebView, Electron under file://) there is no mock WebSocket
// server reachable from the page origin, so createTransport('websocket')
// must hand back the in-page MockTransport instead of a WebSocketTransport
// pointed at a URL nothing is listening on.
describe('createTransport websocket dispatch', () => {
  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('returns MockTransport when running in the Electron embedded shell', async () => {
    vi.stubGlobal('isElectron', true);
    vi.resetModules();
    const { createTransport, MockTransport } = await import('../index');
    const transport = createTransport('websocket');
    expect(transport).toBeInstanceOf(MockTransport);
  });

  it('returns MockTransport when running in the Android WebView shell', async () => {
    vi.stubGlobal('isElectron', undefined);
    vi.stubGlobal('brambleAndroid', true);
    vi.resetModules();
    const { createTransport, MockTransport } = await import('../index');
    const transport = createTransport('websocket');
    expect(transport).toBeInstanceOf(MockTransport);
  });

  it('returns WebSocketTransport pointed at the dev mock server on plain web', async () => {
    vi.stubGlobal('isElectron', undefined);
    vi.stubGlobal('brambleAndroid', undefined);
    vi.resetModules();
    const { createTransport, WebSocketTransport } = await import('../index');
    const transport = createTransport('websocket');
    expect(transport).toBeInstanceOf(WebSocketTransport);
  });
});
