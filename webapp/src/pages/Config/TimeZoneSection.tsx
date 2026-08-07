import { useEffect, useState } from 'react';
import type { TimezoneInfo } from '../../types/bramble';
import { loadTimezone, setTimezone } from '../../store/actions';
import { friendlyErrorFrom } from '../../lib/errors';
import styles from './TimeZoneSection.module.css';

const CUSTOM = '__custom__';

export function TimeZoneSection() {
  const [info, setInfo] = useState<TimezoneInfo | null>(null);
  const [selected, setSelected] = useState('');
  const [custom, setCustom] = useState('');
  const [error, setError] = useState('');
  const [saving, setSaving] = useState(false);

  const refresh = async () => {
    setError('');
    try {
      const next = await loadTimezone();
      setInfo(next);
      const known = next.presets.some((p) => p.spec === next.timezone);
      setSelected(known ? next.timezone : CUSTOM);
      setCustom(known ? '' : next.timezone);
    } catch (e) {
      setError(friendlyErrorFrom(e));
    }
  };

  useEffect(() => {
    void refresh();
    // Read once on mount: the zone only changes when this form or the device
    // itself changes it.
  }, []);

  const save = async (spec: string) => {
    setError('');
    setSaving(true);
    try {
      await setTimezone(spec);
      await refresh();
    } catch (e) {
      setError(friendlyErrorFrom(e));
    } finally {
      setSaving(false);
    }
  };

  const onPick = (value: string) => {
    setSelected(value);
    if (value !== CUSTOM) void save(value);
  };

  if (!info) {
    return (
      <div className={styles.section}>
        {error ? <div className={styles.error}>{error}</div> : <div className={styles.dim}>Reading timezone…</div>}
      </div>
    );
  }

  return (
    <div className={styles.section}>
      {error && <div className={styles.error}>{error}</div>}

      <div className={styles.row}>
        <label className={styles.label} htmlFor="tz-zone">Device zone</label>
        <select
          id="tz-zone"
          aria-label="Device zone"
          value={selected}
          disabled={saving}
          onChange={(e) => onPick(e.target.value)}
        >
          {info.presets.map((p) => (
            <option key={p.spec} value={p.spec}>{p.label}</option>
          ))}
          <option value={CUSTOM}>Custom POSIX TZ…</option>
        </select>
      </div>

      {selected === CUSTOM && (
        <div className={styles.row}>
          <label className={styles.label} htmlFor="tz-custom">POSIX TZ</label>
          <input
            id="tz-custom"
            aria-label="POSIX TZ"
            className={styles.customInput}
            value={custom}
            placeholder="PST8PDT,M3.2.0,M11.1.0"
            onChange={(e) => setCustom(e.target.value)}
          />
          <button disabled={saving || custom.trim() === ''} onClick={() => void save(custom.trim())}>
            {saving ? 'Saving…' : 'Apply'}
          </button>
        </div>
      )}

      <div className={styles.row}>
        <span className={styles.label}>In effect</span>
        <span className={styles.mono}>{info.timezone}</span>
        {!info.configured && <span className={styles.dim}>(default)</span>}
      </div>

      <p className={styles.dim}>
        Sets the zone the node itself renders on its status-bar clock. The node keeps UTC internally and converts
        only when it draws a clock, so this does not change any timestamp on the wire. Times shown in this app
        already follow your browser's zone and are unaffected. A zone that observes daylight saving must spell out
        its transition rules, as in <span className={styles.mono}>PST8PDT,M3.2.0,M11.1.0</span>.
      </p>
    </div>
  );
}
