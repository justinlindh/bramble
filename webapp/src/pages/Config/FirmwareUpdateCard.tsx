import { useEffect, useReducer, useRef, useState } from 'react';
import {
  getOtaOrigin, getOtaStatus, loadStatus, startOtaUpdate, subscribeOtaEvents,
  type OtaOriginInfo,
} from '../../store/actions';
import {
  fetchOtaIndex, hardwareToBoard, relativizeArtifactPath, releasesForBoard,
  compareVersions, appArtifactForBoard, type OtaArtifact, type OtaRelease,
} from '../../lib/otaIndex';
import { useStore } from '../../store';
import { friendlyErrorFrom } from '../../lib/errors';
import { initialOtaFlow, otaFlowNext } from './otaFlow';
import styles from './DeviceManagementSection.module.css';

interface Props {
  ota: OtaOriginInfo;
  onOtaChanged: () => void | Promise<void>;
  // Fired when the user actually starts an install, so the surrounding section
  // can clear stale notices/errors sitting next to this card (finding 10).
  onInstallStart?: () => void;
}

// Turn a raw progress stage into stranger-friendly copy. No jargon: a stranger
// should read "Downloading firmware 45%" not "state: downloading".
function stageLabel(stage: string, percent: number): string {
  switch (stage) {
    case 'starting':
      return 'Starting update';
    case 'downloading':
      return `Downloading firmware ${percent}%`;
    case 'verifying':
      return 'Verifying signature';
    case 'rebooting':
      return 'Node is rebooting';
    case 'reconnecting':
      // A transient connection blip resumed; progress keeps flowing.
      return 'Reconnecting';
    case 'idle':
      // A fallback poll can report the firmware's idle state mid-install
      // (state has not caught up yet); "Idle" reads as stalled, not jargon.
      return 'Working';
    default:
      return stage ? stage.charAt(0).toUpperCase() + stage.slice(1) : 'Working';
  }
}

function publishedLabel(iso: string): string {
  const t = Date.parse(iso);
  return Number.isNaN(t) ? '' : new Date(t).toLocaleDateString();
}

// The user-facing firmware-update journey: pick a version, watch progress, and
// get told plainly whether the node came back on the new firmware. The pure
// transitions live in otaFlow.ts; this owns the timers, the RPC subscription,
// and the release-index fetch.
export function FirmwareUpdateCard({ ota, onOtaChanged, onInstallStart }: Props) {
  const [flow, dispatch] = useReducer(otaFlowNext, undefined, initialOtaFlow);

  const [busy, setBusy] = useState(false);
  const [hardware, setHardware] = useState('');
  const [releases, setReleases] = useState<OtaRelease[]>([]);
  const [indexError, setIndexError] = useState<string | null>(null);
  const [selectedVersion, setSelectedVersion] = useState('');
  const [advancedOpen, setAdvancedOpen] = useState(false);
  const [rawPath, setRawPath] = useState('');

  // Confirm gate: the release awaiting an explicit confirm, plus the downgrade
  // acknowledgement checkbox.
  const [pending, setPending] = useState<{ version: string; artifact: OtaArtifact } | null>(null);
  const [allowDowngrade, setAllowDowngrade] = useState(false);

  const lastEventAt = useRef(0);
  const mounted = useRef(true);
  const connectionState = useStore((s) => s.connectionState);

  // Tracks live-ness for the async poll callbacks below (getOtaOrigin/getOtaStatus
  // can resolve after unmount), so their .then() handlers never setState on a
  // dead component.
  useEffect(() => {
    // Set inside the effect body, not just the cleanup: React 18 StrictMode
    // mounts, unmounts, then remounts in dev, so a cleanup-only reset leaves
    // mounted.current stuck false and silences every guarded poll dispatch.
    mounted.current = true;
    return () => { mounted.current = false; };
  }, []);

  const board = hardware ? hardwareToBoard(hardware) : '';
  const boardReleases = board ? releasesForBoard(releases, board) : [];
  // boardReleases is already sorted newest-first (releasesForBoard), so the
  // update badge is just "is the newest release strictly newer than running".
  const newest = boardReleases[0];
  const update = newest && compareVersions(newest.version, ota.runningVersion ?? '') > 0 ? newest : null;
  const selectedRelease = boardReleases.find((r) => r.version === selectedVersion) ?? null;
  // Canonical bramble.bin selection (finding 1). releasesForBoard already drops
  // releases with no installable app image, so a null here is defensive.
  const selectedArtifact = selectedRelease && board ? appArtifactForBoard(selectedRelease, board) : null;

  const loadIndex = async () => {
    setBusy(true);
    setIndexError(null);
    try {
      let status = useStore.getState().status;
      if (!status) {
        await loadStatus();
        status = useStore.getState().status;
      }
      const hw = status?.hardware ?? '';
      setHardware(hw);
      const rels = await fetchOtaIndex(ota.origin);
      setReleases(rels);
      const forBoard = releasesForBoard(rels, hardwareToBoard(hw));
      setSelectedVersion(forBoard[0]?.version ?? '');
    } catch (e) {
      setReleases([]);
      setIndexError(friendlyErrorFrom(e));
      setAdvancedOpen(true);
    } finally {
      setBusy(false);
    }
  };

  // (Re)load the index whenever the configured origin changes.
  useEffect(() => {
    void loadIndex();
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [ota.origin]);

  // Subscribe once: firmware progress events drive the running phase. The
  // disposer is returned as the effect's cleanup so unmount (or a remount
  // during React StrictMode/HMR) tears down the RPC subscription instead of
  // leaking it.
  useEffect(() => {
    const unsubscribe = subscribeOtaEvents((e) => {
      lastEventAt.current = Date.now();
      dispatch({ kind: 'event', state: e.state, percent: e.percent, error: e.lastError });
    });
    return unsubscribe;
  }, []);

  // A transport drop while installing means the node rebooted into the new
  // image; the machine folds that into the rebooting wait.
  useEffect(() => {
    if (flow.phase === 'running' && connectionState !== 'connected') {
      dispatch({ kind: 'disconnected' });
    }
  }, [flow.phase, connectionState]);

  // Fallback poll: if no event lands for 5s while running, ask getOtaStatus.
  useEffect(() => {
    if (flow.phase !== 'running') return;
    const id = setInterval(async () => {
      if (Date.now() - lastEventAt.current < 5000) return;
      try {
        const st = await getOtaStatus();
        if (!mounted.current) return;
        dispatch({ kind: 'event', state: st.state, percent: st.percent, error: st.lastError });
      } catch {
        if (!mounted.current) return;
        // The node likely rebooted out from under us; treat as reboot.
        dispatch({ kind: 'disconnected' });
      }
    }, 2000);
    return () => clearInterval(id);
  }, [flow.phase]);

  // Reboot wait: poll the version query every 3s, give up at 45s.
  useEffect(() => {
    if (flow.phase !== 'rebooting') return;
    const poll = setInterval(async () => {
      try {
        const info = await getOtaOrigin();
        if (!mounted.current) return;
        const v = info.runningVersion ?? '';
        if (v) {
          // An index-driven install knows its target and verifies it; the
          // advanced raw-path install has no target, so any healthy answer is
          // success.
          dispatch({ kind: 'reconnected', version: flow.targetVersion ? v : '' });
        }
      } catch {
        // Still down; keep polling until the 45s timeout fires.
      }
    }, 3000);
    const timeout = setTimeout(() => dispatch({ kind: 'rebootTimeout' }), 45000);
    return () => {
      clearInterval(poll);
      clearTimeout(timeout);
    };
  }, [flow.phase, flow.targetVersion]);

  const beginInstall = (version: string, path: string | null) => {
    onInstallStart?.();
    setPending(null);
    lastEventAt.current = Date.now();
    dispatch({ kind: 'start', targetVersion: version, prevVersion: ota.runningVersion ?? '' });
    if (path === null) {
      dispatch({ kind: 'event', state: 'failed', percent: 0, error: 'Artifact path is outside the configured origin.' });
      return;
    }
    void (async () => {
      try {
        const r = await startOtaUpdate(path, allowDowngrade);
        if (!r.ok) {
          dispatch({ kind: 'event', state: 'failed', percent: 0, error: r.error ?? r.lastError ?? 'Update could not start.' });
        }
      } catch (e) {
        dispatch({ kind: 'event', state: 'failed', percent: 0, error: friendlyErrorFrom(e) });
      }
    })();
  };

  const handleConfirm = () => {
    if (!pending) return;
    const rel = relativizeArtifactPath(pending.artifact.file, ota.origin);
    beginInstall(pending.version, rel);
  };

  const handleAdvancedInstall = () => {
    const path = rawPath.trim();
    if (!path) return;
    // Raw-path install: version unknown, so no post-reboot version match.
    beginInstall('', path);
  };

  const handleBack = async () => {
    dispatch({ kind: 'reset' });
    setPending(null);
    setAllowDowngrade(false);
    await onOtaChanged();
    await loadIndex();
  };

  const needsDowngradeAck = !!pending && !!ota.versionFloor
    && compareVersions(pending.version, ota.versionFloor) < 0;

  const clampedPercent = Math.max(0, Math.min(100, flow.percent));

  // ─── Running ──────────────────────────────────────────────────────────
  if (flow.phase === 'running') {
    return (
      <>
        <div className={styles.progressTrack}>
          <div className={styles.progressFill} style={{ width: `${clampedPercent}%` }} />
        </div>
        <p className={styles.hint}>{stageLabel(flow.stage, clampedPercent)}</p>
        <p className={styles.muted}>Keep this page open. The node reboots when it finishes.</p>
      </>
    );
  }

  // ─── Rebooting ────────────────────────────────────────────────────────
  if (flow.phase === 'rebooting') {
    return (
      <>
        <div className={styles.progressTrack}>
          <div className={styles.progressFill} style={{ width: '100%' }} />
        </div>
        <p className={styles.hint}>Checking the node. Waiting for it to come back...</p>
      </>
    );
  }

  // ─── Done ─────────────────────────────────────────────────────────────
  if (flow.phase === 'done') {
    const shown = flow.resultVersion ?? flow.targetVersion;
    return (
      <>
        <p className={styles.notice}>Updated to {shown || 'the new firmware'}.</p>
        <div className={styles.row}>
          <button className={styles.primaryBtn} onClick={() => void handleBack()}>Back</button>
        </div>
      </>
    );
  }

  // ─── Failed ───────────────────────────────────────────────────────────
  if (flow.phase === 'failed') {
    return (
      <>
        <p className={styles.error}>{flow.error ?? 'The update did not finish.'}</p>
        <div className={styles.row}>
          <button className={styles.primaryBtn} onClick={() => void handleBack()}>Try again</button>
        </div>
      </>
    );
  }

  // ─── Confirming ───────────────────────────────────────────────────────
  if (pending) {
    return (
      <>
        {needsDowngradeAck ? (
          <>
            <p className={styles.hint}>
              Install {pending.version}? This is older than the node's rollback floor {ota.versionFloor}.
              The node reboots when it finishes.
            </p>
            <label className={styles.row}>
              <input
                type="checkbox"
                checked={allowDowngrade}
                onChange={(e) => setAllowDowngrade(e.target.checked)}
              />
              <span>Allow downgrade</span>
            </label>
          </>
        ) : (
          <p className={styles.hint}>Install {pending.version}? The node reboots when it finishes.</p>
        )}
        <div className={styles.row}>
          <button
            className={styles.dangerBtn}
            onClick={handleConfirm}
            disabled={needsDowngradeAck && !allowDowngrade}
          >
            Install
          </button>
          <button className={styles.ghostBtn} onClick={() => { setPending(null); setAllowDowngrade(false); }}>
            Cancel
          </button>
        </div>
      </>
    );
  }

  // ─── Idle ─────────────────────────────────────────────────────────────
  return (
    <>
      {update && (
        <p>
          <span className={styles.updateBadge}>Update available: {update.version}</span>
        </p>
      )}

      {indexError && <p className={styles.error}>{indexError}</p>}

      {boardReleases.length > 0 && (
        <>
          <div className={styles.row}>
            <span className={styles.label}>Version</span>
            <select
              className={styles.input}
              value={selectedVersion}
              onChange={(e) => setSelectedVersion(e.target.value)}
            >
              {boardReleases.map((r) => {
                const date = publishedLabel(r.publishedAt);
                return (
                  <option key={r.version} value={r.version}>
                    {r.version}{r.channel ? ` (${r.channel})` : ''}{date ? ` ${date}` : ''}
                  </option>
                );
              })}
            </select>
          </div>
          {selectedArtifact?.notes && <p className={styles.muted}>{selectedArtifact.notes}</p>}
          <div className={styles.row}>
            <button
              className={styles.primaryBtn}
              onClick={() => {
                if (!selectedRelease || !selectedArtifact) return;
                setAllowDowngrade(false);
                setPending({ version: selectedRelease.version, artifact: selectedArtifact });
              }}
              disabled={busy || !selectedArtifact}
            >
              Update
            </button>
          </div>
        </>
      )}

      {!indexError && boardReleases.length === 0 && !busy && (
        <p className={styles.muted}>No published updates for this node yet.</p>
      )}

      <details
        open={advancedOpen}
        onToggle={(e) => setAdvancedOpen((e.target as HTMLDetailsElement).open)}
      >
        <summary className={styles.hint}>Advanced: install by artifact path</summary>
        <div className={styles.row}>
          <span className={styles.label}>Artifact path</span>
          <input
            className={styles.input}
            type="text"
            value={rawPath}
            placeholder="stable/v1.4.0/heltec-v3/bramble.bin"
            onChange={(e) => setRawPath(e.target.value)}
            autoComplete="off"
          />
          <button
            className={styles.dangerBtn}
            onClick={handleAdvancedInstall}
            disabled={busy || !rawPath.trim()}
          >
            Start update
          </button>
        </div>
        <p className={styles.muted}>The node reboots on a successful update.</p>
      </details>
    </>
  );
}
