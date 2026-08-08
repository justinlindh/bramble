import { useEffect, useState } from 'react';
import {
  getAuthToken, setAuthToken,
  getAllowedOrigins, setAllowedOrigins,
  getOtaOrigin, setOtaOrigin, resetOtaOrigin,
  getBleSecurity, setBlePasskey,
  type OtaOriginInfo, type BleSecurityInfo,
} from '../../store/actions';
import { friendlyErrorFrom } from '../../lib/errors';
import { FirmwareUpdateCard } from './FirmwareUpdateCard';
import styles from './DeviceManagementSection.module.css';

const REPAIR_NOTICE = 'Saved. All paired devices were unpaired and must pair again with the new code.';

// BLE pairing security: how new clients pair over Bluetooth. Some boards
// display a random code on their own screen (nothing to configure here);
// others need a fixed passkey set here since they have no display to show
// one. Self-loads on mount so it works standalone within the section's
// load-on-expand gate, mirroring the getAuthToken load flow above.
export function BleSecurityCard() {
  const [info, setInfo] = useState<BleSecurityInfo | null>(null);
  const [passkey, setPasskeyDraft] = useState('');
  const [loading, setLoading] = useState(false);
  const [notice, setNotice] = useState<string | null>(null);
  const [error, setError] = useState<string | null>(null);

  const load = async () => {
    try {
      const i = await getBleSecurity();
      setInfo(i);
    } catch (e) {
      setError(friendlyErrorFrom(e));
    }
  };

  useEffect(() => { load(); }, []);

  const applyPasskey = async (next: string | null) => {
    setLoading(true);
    setError(null);
    setNotice(null);
    try {
      const r = await setBlePasskey(next);
      if (!r.ok) { setError(r.error ?? 'Could not set passkey.'); return; }
      await load();
      setNotice(REPAIR_NOTICE);
      setPasskeyDraft('');
    } catch (e) {
      setError(friendlyErrorFrom(e));
    } finally {
      setLoading(false);
    }
  };

  if (!info) {
    return (
      <div className={styles.subsection}>
        <h3 className={styles.subheading}>Bluetooth Pairing</h3>
        {error && <p className={styles.error}>{error}</p>}
      </div>
    );
  }

  const validPasskey = /^[0-9]{6}$/.test(passkey);

  return (
    <div className={styles.subsection}>
      <h3 className={styles.subheading}>Bluetooth Pairing</h3>
      {info.mode === 'passkey-display' ? (
        <p className={styles.hint}>
          This device shows a random pairing code on its screen when a new client pairs.
          Nothing to configure.
        </p>
      ) : (
        <>
          <p className={styles.hint}>
            {info.staticPasskeySet
              ? 'A fixed passkey is set. New devices pair using it.'
              : 'No fixed passkey is set. New devices pair without confirmation.'}
          </p>
          <div className={styles.row}>
            <span className={styles.label}>New passkey</span>
            <input
              className={styles.input}
              type="text"
              inputMode="numeric"
              pattern="[0-9]{6}"
              maxLength={6}
              value={passkey}
              onChange={(e) => setPasskeyDraft(e.target.value)}
              placeholder="6 digits"
              autoComplete="off"
            />
          </div>
          <div className={styles.row}>
            <button
              className={styles.primaryBtn}
              onClick={() => applyPasskey(passkey)}
              disabled={loading || !validPasskey}
            >
              Save passkey
            </button>
            {info.staticPasskeySet && (
              <button className={styles.ghostBtn} onClick={() => applyPasskey(null)} disabled={loading}>
                Clear
              </button>
            )}
          </div>
        </>
      )}
      {notice && <p className={styles.notice}>{notice}</p>}
      {error && <p className={styles.error}>{error}</p>}
    </div>
  );
}

// Issue #95: the web client had no UI for the device auth token, the WS Origin
// allowlist, or OTA, even though the firmware exposes RPCs for all three. This
// section surfaces them with stranger-friendly copy so onboarding does not
// depend on the CLI.
export function DeviceManagementSection() {
  const [loaded, setLoaded] = useState(false);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  // Auth token
  const [authEnabled, setAuthEnabled] = useState(false);
  const [newToken, setNewToken] = useState('');
  const [showToken, setShowToken] = useState(false);
  const [tokenNotice, setTokenNotice] = useState<string | null>(null);

  // Allowed origins
  const [origins, setOrigins] = useState<string[]>([]);
  const [newOrigin, setNewOrigin] = useState('');

  // OTA
  const [ota, setOta] = useState<OtaOriginInfo | null>(null);
  const [otaNotice, setOtaNotice] = useState<string | null>(null);
  // Draft origin for the input, kept separate from the SAVED ota.origin so the
  // card (which loads its index and relativizes paths against ota.origin) never
  // sees an unsaved keystroke-in-progress origin (finding 6).
  const [originDraft, setOriginDraft] = useState('');

  const loadAll = async () => {
    setLoading(true);
    setError(null);
    try {
      const [a, o, t] = await Promise.all([getAuthToken(), getAllowedOrigins(), getOtaOrigin()]);
      setAuthEnabled(a.enabled);
      setOrigins(o);
      setOta(t);
      setOriginDraft(t.origin);
      setLoaded(true);
    } catch (e) {
      setError(friendlyErrorFrom(e));
    } finally {
      setLoading(false);
    }
  };

  const handleRotateToken = async () => {
    setLoading(true);
    setTokenNotice(null);
    setError(null);
    try {
      await setAuthToken(newToken);
      setAuthEnabled(newToken.length > 0);
      setTokenNotice(newToken.length > 0
        ? 'Token updated. Other devices must reconnect with the new token.'
        : 'Authentication disabled. Anyone on the network can connect.');
      setNewToken('');
    } catch (e) {
      setError(friendlyErrorFrom(e));
    } finally {
      setLoading(false);
    }
  };

  const handleAddOrigin = async () => {
    const next = [...origins, newOrigin.trim()].filter(Boolean);
    setLoading(true);
    setError(null);
    try {
      await setAllowedOrigins(next);
      setOrigins(next);
      setNewOrigin('');
    } catch (e) {
      setError(friendlyErrorFrom(e));
    } finally {
      setLoading(false);
    }
  };

  const handleRemoveOrigin = async (origin: string) => {
    const next = origins.filter(o => o !== origin);
    setLoading(true);
    setError(null);
    try {
      await setAllowedOrigins(next);
      setOrigins(next);
    } catch (e) {
      setError(friendlyErrorFrom(e));
    } finally {
      setLoading(false);
    }
  };

  const refreshOta = async () => {
    const t = await getOtaOrigin();
    setOta(t);
    setOriginDraft(t.origin);
  };

  const handleSetOtaOrigin = async () => {
    setLoading(true);
    setOtaNotice(null);
    setError(null);
    try {
      const r = await setOtaOrigin(originDraft);
      if (!r.ok) { setError(r.error ?? 'Could not set update origin.'); return; }
      await refreshOta();
      setOtaNotice('Update origin saved.');
    } catch (e) {
      setError(friendlyErrorFrom(e));
    } finally {
      setLoading(false);
    }
  };

  const handleResetOtaOrigin = async () => {
    setLoading(true);
    setError(null);
    try {
      await resetOtaOrigin();
      await refreshOta();
    } catch (e) {
      setError(friendlyErrorFrom(e));
    } finally {
      setLoading(false);
    }
  };

  if (!loaded) {
    return (
      <div className={styles.section}>
        <button className={styles.loadBtn} onClick={loadAll} disabled={loading}>
          {loading ? 'Loading...' : 'Load Device Management'}
        </button>
        {error && <p className={styles.error}>{error}</p>}
      </div>
    );
  }

  return (
    <div className={styles.section}>
      {error && <p className={styles.error}>{error}</p>}

      {/* Authentication */}
      <div className={styles.subsection}>
        <h3 className={styles.subheading}>Authentication</h3>
        <p className={styles.hint}>
          {authEnabled
            ? 'This node requires a token. Devices connect with it over WiFi.'
            : 'This node is open: anyone on the network can connect. Set a token to require one.'}
          {' '}You can read the current token over a USB cable with the bramble CLI (the pair command), or set a new one here.
        </p>
        <div className={styles.row}>
          <span className={styles.label}>New token</span>
          <input
            className={styles.input}
            type={showToken ? 'text' : 'password'}
            value={newToken}
            placeholder="16+ characters, or empty to disable"
            onChange={(e) => setNewToken(e.target.value)}
            autoComplete="off"
          />
          <button className={styles.ghostBtn} type="button" onClick={() => setShowToken(s => !s)}>
            {showToken ? 'Hide' : 'Show'}
          </button>
        </div>
        <div className={styles.row}>
          <button className={styles.primaryBtn} onClick={handleRotateToken} disabled={loading}>
            {newToken.length > 0 ? 'Set token' : 'Disable auth'}
          </button>
        </div>
        {tokenNotice && <p className={styles.notice}>{tokenNotice}</p>}
      </div>

      {/* Allowed origins */}
      <div className={styles.subsection}>
        <h3 className={styles.subheading}>Allowed Web Origins</h3>
        <p className={styles.hint}>
          Browser pages from these origins may open a WebSocket to this node. The node's own
          address is always allowed; add extras here only if you host the web client elsewhere.
        </p>
        {origins.length === 0
          ? <p className={styles.muted}>No extra origins. Same-origin pages are allowed by default.</p>
          : (
            <ul className={styles.list}>
              {origins.map(o => (
                <li key={o} className={styles.listItem}>
                  <code>{o}</code>
                  <button className={styles.ghostBtn} onClick={() => handleRemoveOrigin(o)} disabled={loading}>Remove</button>
                </li>
              ))}
            </ul>
          )}
        <div className={styles.row}>
          <input
            className={styles.input}
            type="text"
            value={newOrigin}
            placeholder="https://app.example.com"
            onChange={(e) => setNewOrigin(e.target.value)}
            autoComplete="off"
          />
          <button className={styles.primaryBtn} onClick={handleAddOrigin} disabled={loading || !newOrigin.trim()}>Add</button>
        </div>
      </div>

      <BleSecurityCard />

      {/* Firmware update */}
      {ota && (
        <div className={styles.subsection}>
          <h3 className={styles.subheading}>Firmware Update</h3>
          <p className={styles.hint}>
            Running {ota.runningVersion ?? 'unknown'}
            {ota.versionFloor ? ` (rollback floor ${ota.versionFloor})` : ''}.
            Updates are downloaded from the allowlisted origin below and must be signed.
          </p>
          <div className={styles.row}>
            <span className={styles.label}>Update origin</span>
            <input
              className={styles.input}
              type="text"
              value={originDraft}
              onChange={(e) => setOriginDraft(e.target.value)}
              autoComplete="off"
            />
          </div>
          <div className={styles.row}>
            <button className={styles.primaryBtn} onClick={handleSetOtaOrigin} disabled={loading}>Save origin</button>
            {ota.overridden && (
              <button className={styles.ghostBtn} onClick={handleResetOtaOrigin} disabled={loading}>
                Reset to default
              </button>
            )}
          </div>
          {otaNotice && <p className={styles.notice}>{otaNotice}</p>}
          <FirmwareUpdateCard
            ota={ota}
            onOtaChanged={refreshOta}
            onInstallStart={() => { setError(null); setOtaNotice(null); }}
          />
        </div>
      )}
    </div>
  );
}
