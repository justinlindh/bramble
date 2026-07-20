import { describe, it, expect, vi } from 'vitest';
import { runUpdateCheck, type UpdaterDeps } from '../../electron/updateCheck';
import type { GithubRelease } from '../../electron/updatePolicy';

function makeDeps(overrides: Partial<UpdaterDeps> = {}): UpdaterDeps {
  return {
    currentVersion: '1.4.0',
    platform: 'linux',
    env: { APPIMAGE: '/tmp/Bramble.AppImage' },
    isPackaged: true,
    fetchReleases: vi.fn(async (): Promise<GithubRelease[]> => [{ tag_name: 'webapp-v1.5.0' }]),
    log: vi.fn(),
    confirmDownload: vi.fn(async () => true),
    confirmRestart: vi.fn(async () => true),
    notifyManual: vi.fn(async () => {}),
    startInAppUpdate: vi.fn(),
    applyUpdate: vi.fn(),
    ...overrides,
  };
}

describe('runUpdateCheck', () => {
  it('skips entirely when the app is not packaged', async () => {
    const deps = makeDeps({ isPackaged: false, fetchReleases: vi.fn() });
    const decision = await runUpdateCheck(deps);
    expect(decision.kind).toBe('up-to-date');
    expect(deps.fetchReleases).not.toHaveBeenCalled();
  });

  it('treats a failed release fetch as up-to-date without prompting', async () => {
    const deps = makeDeps({ fetchReleases: vi.fn(async () => { throw new Error('offline'); }) });
    const decision = await runUpdateCheck(deps);
    expect(decision.kind).toBe('up-to-date');
    expect(deps.confirmDownload).not.toHaveBeenCalled();
    expect(deps.log).toHaveBeenCalledWith('update check skipped: release fetch failed', expect.any(Error));
  });

  it('does nothing when already up to date', async () => {
    const deps = makeDeps({ currentVersion: '1.5.0' });
    const decision = await runUpdateCheck(deps);
    expect(decision.kind).toBe('up-to-date');
    expect(deps.confirmDownload).not.toHaveBeenCalled();
    expect(deps.startInAppUpdate).not.toHaveBeenCalled();
  });

  it('starts the in-app update after the user consents', async () => {
    const deps = makeDeps();
    const decision = await runUpdateCheck(deps);
    expect(decision.kind).toBe('in-app');
    expect(deps.confirmDownload).toHaveBeenCalledWith('1.5.0');
    expect(deps.startInAppUpdate).toHaveBeenCalledWith(
      'https://github.com/justinlindh/bramble/releases/download/webapp-v1.5.0/',
      expect.any(Function),
    );
    expect(deps.notifyManual).not.toHaveBeenCalled();
  });

  it('does not start an update when the user declines', async () => {
    const deps = makeDeps({ confirmDownload: vi.fn(async () => false) });
    await runUpdateCheck(deps);
    expect(deps.startInAppUpdate).not.toHaveBeenCalled();
  });

  it('applies the update when the user confirms the restart', async () => {
    let downloadedCb: (() => void) | undefined;
    const deps = makeDeps({
      startInAppUpdate: vi.fn((_feedUrl: string, onDownloaded: () => void) => {
        downloadedCb = onDownloaded;
      }),
    });
    await runUpdateCheck(deps);
    expect(downloadedCb).toBeTypeOf('function');
    downloadedCb?.();
    // The restart prompt resolves on a microtask; flush it.
    await Promise.resolve();
    await Promise.resolve();
    expect(deps.confirmRestart).toHaveBeenCalledWith('1.5.0');
    expect(deps.applyUpdate).toHaveBeenCalledTimes(1);
  });

  it('does not apply the update when the user postpones the restart', async () => {
    let downloadedCb: (() => void) | undefined;
    const deps = makeDeps({
      confirmRestart: vi.fn(async () => false),
      startInAppUpdate: vi.fn((_feedUrl: string, onDownloaded: () => void) => {
        downloadedCb = onDownloaded;
      }),
    });
    await runUpdateCheck(deps);
    downloadedCb?.();
    await Promise.resolve();
    await Promise.resolve();
    expect(deps.applyUpdate).not.toHaveBeenCalled();
  });

  it('notifies without downloading on a manual-install platform', async () => {
    const deps = makeDeps({ platform: 'darwin', env: {} });
    const decision = await runUpdateCheck(deps);
    expect(decision.kind).toBe('manual');
    expect(deps.notifyManual).toHaveBeenCalledWith('1.5.0', expect.stringContaining('github.com'));
    expect(deps.startInAppUpdate).not.toHaveBeenCalled();
  });

  it('notifies (not in-app) for a Linux deb/pacman install with no APPIMAGE', async () => {
    const deps = makeDeps({ env: {} });
    const decision = await runUpdateCheck(deps);
    expect(decision.kind).toBe('manual');
    expect(deps.startInAppUpdate).not.toHaveBeenCalled();
  });
});
