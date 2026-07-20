// Electron glue for the desktop auto-updater. This is the only updater file
// that imports electron / electron-updater; the decision logic lives in the
// electron-free updatePolicy.ts and updateCheck.ts, which are unit tested. Keep
// this file thin: it builds the real dependencies and hands them to
// runUpdateCheck.

import { app, BrowserWindow, dialog, net, shell } from 'electron';
import electronUpdater from 'electron-updater';
import { runUpdateCheck, type UpdaterDeps } from './updateCheck';
import { GITHUB_OWNER, GITHUB_REPO, type GithubRelease } from './updatePolicy';

// electron-updater's `autoUpdater` is a lazy getter: touching it constructs a
// platform updater that immediately reads app.getVersion(). Resolve it only
// inside the functions that actually run an update, never at module load, so
// merely importing this file (transitively, via electron/main.ts in unit tests)
// does not require a live Electron app.
function getAutoUpdater(): typeof electronUpdater.autoUpdater {
  return electronUpdater.autoUpdater;
}

// The app checks for updates once per launch, a few seconds after the window
// is ready so startup is never blocked. Guards against a double check if the
// launcher is called more than once (e.g. macOS activate re-creating a window).
let checkStarted = false;

// GitHub asks every API client to send a User-Agent; requests without one are
// rejected. Fetch a page of recent releases (all components share the repo, so
// this spans firmware/protocol/sim/webapp tags; pickLatestWebappRelease filters
// to webapp).
async function fetchReleases(): Promise<GithubRelease[]> {
  const url = `https://api.github.com/repos/${GITHUB_OWNER}/${GITHUB_REPO}/releases?per_page=30`;
  const resp = await net.fetch(url, {
    headers: {
      Accept: 'application/vnd.github+json',
      'User-Agent': `Bramble-Desktop/${app.getVersion()}`,
    },
  });
  if (!resp.ok) {
    throw new Error(`GitHub releases request failed: ${resp.status}`);
  }
  const body: unknown = await resp.json();
  if (!Array.isArray(body)) {
    throw new Error('GitHub releases response was not an array');
  }
  return body as GithubRelease[];
}

function buildDeps(window: BrowserWindow | null): UpdaterDeps {
  const log: UpdaterDeps['log'] = (message, error) => {
    if (error !== undefined) console.error(`[updater] ${message}`, error);
    else console.log(`[updater] ${message}`);
  };

  async function confirm(
    title: string,
    detail: string,
    confirmLabel: string,
  ): Promise<boolean> {
    const opts = {
      type: 'info' as const,
      buttons: [confirmLabel, 'Later'],
      defaultId: 0,
      cancelId: 1,
      title: 'Bramble',
      message: title,
      detail,
      noLink: true,
    };
    const result = window
      ? await dialog.showMessageBox(window, opts)
      : await dialog.showMessageBox(opts);
    return result.response === 0;
  }

  return {
    currentVersion: app.getVersion(),
    platform: process.platform,
    env: process.env,
    isPackaged: app.isPackaged,
    fetchReleases,
    log,
    confirmDownload: (version) =>
      confirm(
        `Bramble ${version} is available`,
        'A newer desktop app is available. Download and install it now?',
        'Download and install',
      ),
    confirmRestart: (version) =>
      confirm(
        `Bramble ${version} is ready to install`,
        'The update has been downloaded. Restart Bramble now to finish installing?',
        'Restart now',
      ),
    notifyManual: async (version, releaseUrl) => {
      const wantsOpen = await confirm(
        `Bramble ${version} is available`,
        'This install updates from the download page (system package or unsigned build). Open the releases page to get the new version?',
        'Open releases page',
      );
      if (wantsOpen) await shell.openExternal(releaseUrl);
    },
    startInAppUpdate: (feedUrl, onDownloaded) => {
      const autoUpdater = getAutoUpdater();
      autoUpdater.logger = {
        info: (m: unknown) => log(String(m)),
        warn: (m: unknown) => log(String(m)),
        error: (m: unknown) => log('electron-updater error', m),
        debug: (_m: unknown) => {},
      };
      autoUpdater.autoDownload = false;
      autoUpdater.autoInstallOnAppQuit = false;
      autoUpdater.setFeedURL({ provider: 'generic', url: feedUrl });
      autoUpdater.removeAllListeners();
      autoUpdater.on('update-available', () => {
        autoUpdater.downloadUpdate().catch((error) => log('downloadUpdate failed', error));
      });
      autoUpdater.on('update-downloaded', () => onDownloaded());
      autoUpdater.on('error', (error) => log('electron-updater error', error));
      autoUpdater.checkForUpdates().catch((error) => log('checkForUpdates failed', error));
    },
    applyUpdate: () => {
      // Defer past the current tick so the dialog closes cleanly before the app
      // quits to install.
      setImmediate(() => getAutoUpdater().quitAndInstall());
    },
  };
}

/**
 * Kicks off a single update check shortly after launch. Safe to call once the
 * main window exists; no-ops on repeat calls and in unpackaged/dev runs.
 */
export function scheduleUpdateCheck(window: BrowserWindow | null): void {
  if (checkStarted) return;
  checkStarted = true;
  setTimeout(() => {
    void runUpdateCheck(buildDeps(window)).catch((error) => {
      console.error('[updater] update check crashed', error);
    });
  }, 3000);
}
