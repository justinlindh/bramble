// Orchestration for a single desktop update check. Imports only the pure
// updatePolicy helpers, never electron or electron-updater, so it is unit
// testable with fake dependencies (see test/electron/updateCheck.test.ts).
// electron/updater.ts builds the real dependencies and calls runUpdateCheck.

import {
  decideUpdate,
  pickLatestWebappRelease,
  updateCapability,
  type GithubRelease,
  type UpdateDecision,
} from './updatePolicy';

/**
 * Everything runUpdateCheck needs from the outside world, injected so the
 * control flow can be exercised without a running Electron process.
 */
export interface UpdaterDeps {
  /** Version of the running app (app.getVersion()). */
  currentVersion: string;
  /** process.platform. */
  platform: NodeJS.Platform;
  /** process.env (only APPIMAGE is read). */
  env: Record<string, string | undefined>;
  /** app.isPackaged: dev/unpacked runs skip the check entirely. */
  isPackaged: boolean;
  /** Fetches the repository's recent releases from the GitHub REST API. */
  fetchReleases: () => Promise<GithubRelease[]>;
  /** Structured logging sink (diagnostics only; never surfaced to the user). */
  log: (message: string, error?: unknown) => void;
  /** Asks the user whether to download+install an in-app update. */
  confirmDownload: (version: string) => Promise<boolean>;
  /** Asks the user whether to restart now to apply a downloaded update. */
  confirmRestart: (version: string) => Promise<boolean>;
  /** Tells the user an update exists but must be installed from the browser. */
  notifyManual: (version: string, releaseUrl: string) => Promise<void>;
  /**
   * Points electron-updater at feedUrl and starts the download. onDownloaded
   * fires once the update is staged and ready to install.
   */
  startInAppUpdate: (feedUrl: string, onDownloaded: () => void) => void;
  /** Quits and installs the staged update. */
  applyUpdate: () => void;
}

/**
 * Runs one check-for-updates cycle. Returns the decision that was acted on so
 * callers (and tests) can observe the outcome. Never throws: a failed fetch is
 * logged and treated as up-to-date so a flaky network at launch never nags or
 * crashes the app.
 */
export async function runUpdateCheck(deps: UpdaterDeps): Promise<UpdateDecision> {
  if (!deps.isPackaged) {
    deps.log('update check skipped: app is not packaged');
    return { kind: 'up-to-date' };
  }

  let releases: GithubRelease[];
  try {
    releases = await deps.fetchReleases();
  } catch (error) {
    deps.log('update check skipped: release fetch failed', error);
    return { kind: 'up-to-date' };
  }

  const latest = pickLatestWebappRelease(releases);
  const capability = updateCapability(deps.platform, deps.env);
  const decision = decideUpdate({ currentVersion: deps.currentVersion, latest, capability });

  switch (decision.kind) {
    case 'up-to-date':
      deps.log(`update check: up to date (current ${deps.currentVersion})`);
      return decision;

    case 'manual':
      deps.log(`update available (manual install): ${decision.version}`);
      try {
        await deps.notifyManual(decision.version, decision.releaseUrl);
      } catch (error) {
        deps.log('manual update notification failed', error);
      }
      return decision;

    case 'in-app': {
      deps.log(`update available (in-app): ${decision.version}`);
      let wantsDownload = false;
      try {
        wantsDownload = await deps.confirmDownload(decision.version);
      } catch (error) {
        deps.log('download prompt failed', error);
        return decision;
      }
      if (!wantsDownload) {
        deps.log('user declined the in-app update');
        return decision;
      }
      deps.startInAppUpdate(decision.feedUrl, () => {
        void (async () => {
          let wantsRestart = false;
          try {
            wantsRestart = await deps.confirmRestart(decision.version);
          } catch (error) {
            deps.log('restart prompt failed', error);
            return;
          }
          if (wantsRestart) deps.applyUpdate();
          else deps.log('user postponed the restart; update applies on next quit');
        })();
      });
      return decision;
    }
  }
}
