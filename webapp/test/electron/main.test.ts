import { beforeAll, describe, expect, it, vi } from 'vitest';

/**
 * Pins the Electron permission allowlist (issue: nothing in CI exercised
 * ALLOWED_PERMISSIONS or the handlers that read from it). A regression here
 * is exactly the kind that ships silently: dropping 'media' breaks the QR
 * scanner's getUserMedia call, and adding something like 'geolocation' or
 * 'notifications' would widen what the packaged desktop app grants.
 *
 * electron/main.ts runs its whole module body (including
 * app.whenReady().then(...)) on import, so 'electron', '@electron-toolkit/utils'
 * and './discovery' are mocked here to capture the handlers main.ts registers
 * without needing a real Electron runtime.
 */

type PermissionCheckHandler = (webContents: unknown, permission: string) => boolean;
type PermissionRequestHandler = (
  webContents: unknown,
  permission: string,
  callback: (granted: boolean) => void
) => void;

let capturedCheckHandler: PermissionCheckHandler | undefined;
let capturedRequestHandler: PermissionRequestHandler | undefined;

const defaultSession = {
  setPermissionCheckHandler: vi.fn((fn: PermissionCheckHandler) => {
    capturedCheckHandler = fn;
  }),
  setPermissionRequestHandler: vi.fn((fn: PermissionRequestHandler) => {
    capturedRequestHandler = fn;
  }),
  setDevicePermissionHandler: vi.fn(),
  on: vi.fn(),
};

class FakeBrowserWindow {
  webContents = {
    on: vi.fn(),
    setWindowOpenHandler: vi.fn(),
    send: vi.fn(),
    toggleDevTools: vi.fn(),
  };
  on = vi.fn();
  setTitle = vi.fn();
  loadFile = vi.fn();
  loadURL = vi.fn();
  static getAllWindows(): FakeBrowserWindow[] {
    return [];
  }
}

vi.mock('electron', () => ({
  app: {
    commandLine: { appendSwitch: vi.fn() },
    whenReady: vi.fn(() => Promise.resolve()),
    on: vi.fn(),
    getVersion: vi.fn(() => '0.0.0-test'),
    quit: vi.fn(),
  },
  BrowserWindow: FakeBrowserWindow,
  ipcMain: { on: vi.fn(), handle: vi.fn() },
  Menu: { setApplicationMenu: vi.fn() },
  net: { fetch: vi.fn() },
  session: { defaultSession },
  shell: { openExternal: vi.fn() },
}));

vi.mock('@electron-toolkit/utils', () => ({ is: { dev: false } }));

vi.mock('../../electron/discovery', () => ({
  startDiscovery: vi.fn(),
  stopDiscovery: vi.fn(),
}));

describe('Electron permission allowlist', () => {
  let ALLOWED_PERMISSIONS: Set<string>;

  beforeAll(async () => {
    const main = await import('../../electron/main');
    ALLOWED_PERMISSIONS = main.ALLOWED_PERMISSIONS;
    // app.whenReady() resolves a real Promise; its .then callback (which
    // calls createWindow() and registers the handlers under test) runs as a
    // microtask, so flush the microtask queue before asserting.
    await Promise.resolve();
    await Promise.resolve();
    await Promise.resolve();
  });

  it('pins the exact allowed permission set', () => {
    expect([...ALLOWED_PERMISSIONS].sort()).toEqual(
      ['bluetooth', 'clipboard-sanitized-write', 'media', 'serial'].sort()
    );
  });

  it('registers a permission check handler that reads from the allowlist', () => {
    expect(capturedCheckHandler).toBeDefined();
    const check = capturedCheckHandler!;
    expect(check({}, 'serial')).toBe(true);
    expect(check({}, 'bluetooth')).toBe(true);
    expect(check({}, 'media')).toBe(true);
    expect(check({}, 'clipboard-sanitized-write')).toBe(true);
  });

  it('denies geolocation and notifications through the check handler', () => {
    const check = capturedCheckHandler!;
    expect(check({}, 'geolocation')).toBe(false);
    expect(check({}, 'notifications')).toBe(false);
    expect(check({}, 'midi')).toBe(false);
    expect(check({}, 'idle-detection')).toBe(false);
  });

  it('registers a permission request handler wired to the same allowlist', () => {
    expect(capturedRequestHandler).toBeDefined();
    const request = capturedRequestHandler!;

    const mediaResult = vi.fn();
    request({}, 'media', mediaResult);
    expect(mediaResult).toHaveBeenCalledWith(true);

    const geoResult = vi.fn();
    request({}, 'geolocation', geoResult);
    expect(geoResult).toHaveBeenCalledWith(false);
  });
});
