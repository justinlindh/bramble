import { useEffect, useMemo, useState } from 'react';
import type {
  LocationConfig,
  LocationTier,
  LocationSource,
  LocationContactRule,
  LocationChannelTarget,
  Neighbor,
  Channel,
} from '../../types/bramble';
import { setGpsEnabled, setLocationConfig } from '../../store/actions';
import { useStore } from '../../store';
import { IconLocation, IconLocationOff } from '../../components/Icons';
import { AddressLabel } from '../../components/AddressLabel';
import { formatAddrHex, formatAddrShort } from '../../utils/address';
import { tryParseAddr } from '../../lib/addr';
import { friendlyErrorFrom } from '../../lib/errors';
import styles from './LocationSection.module.css';

interface LocationSectionProps {
  location: LocationConfig;
  neighbors: Neighbor[];
  channels: Channel[];
  gpsAvailable?: boolean;
  gpsEnabled?: boolean;
}

const TIER_OPTIONS: Array<{ value: LocationTier; label: string }> = [
  { value: 'off', label: 'Off (share nothing)' },
  { value: 'presence', label: 'Presence only' },
  { value: 'coarse', label: 'Zone (a few hundred metres)' },
  { value: 'full', label: 'Exact coordinates' },
];

const SOURCE_OPTIONS: Array<{ value: LocationSource; label: string }> = [
  { value: 'hybrid', label: 'Hybrid (GPS + manual fallback)' },
  { value: 'gps', label: 'GPS only' },
  { value: 'manual', label: 'Manual only' },
];

const TIER_DESCRIPTIONS: Record<LocationTier, string> = {
  off: 'Location not shared.',
  presence: 'Online status only; no coordinates.',
  coarse: 'A quantized cell, about 330 m north-south by 670 m east-west at the equator and narrower east-west further from it. Peers see the cell, not a point inside it.',
  full: 'Precise GPS coordinates.',
};

const DEFAULT_INTERVAL = 300;

// Canonical 8-char uppercase hex form of a contact address the user typed, or
// null when it is not 1 to 8 hex digits (optional 0x, trailing garbage
// rejected). tryParseAddr is lib/addr's strict validator for typed addresses;
// formatAddrHex is its formatting counterpart.
export function normalizeAddress(raw: string): string | null {
  const addr = tryParseAddr(raw);
  return addr === null ? null : formatAddrHex(addr);
}

export function LocationSection({ location, neighbors, channels, gpsAvailable = false, gpsEnabled }: LocationSectionProps) {
  const [error, setError] = useState('');
  const [saving, setSaving] = useState(false);
  const [gpsOn, setGpsOn] = useState(gpsEnabled ?? true);

  const [enabled, setEnabled] = useState(location.enabled ?? false);
  const [tier, setTier] = useState<LocationTier>(location.default_tier ?? location.tier ?? 'coarse');
  const [interval, setInterval] = useState(location.interval_s ?? DEFAULT_INTERVAL);
  const [source, setSource] = useState<LocationSource>(location.source ?? 'hybrid');
  const [contactRules, setContactRules] = useState<LocationContactRule[]>(location.contact_rules ?? []);
  const [channelTargets, setChannelTargets] = useState<LocationChannelTarget[]>(location.channel_targets ?? []);

  const [newContactAddress, setNewContactAddress] = useState('');
  const [newChannelTarget, setNewChannelTarget] = useState<number>(channels[0]?.index ?? 0);

  const peerNames = useStore(s => s.peerNames);

  const resolveLabel = (hexAddr: string): string | undefined => {
    const num = parseInt(hexAddr, 16);
    return peerNames.get(num);
  };

  useEffect(() => {
    setEnabled(location.enabled ?? false);
    setTier(location.default_tier ?? location.tier ?? 'coarse');
    setInterval(location.interval_s ?? DEFAULT_INTERVAL);
    setSource(location.source ?? 'hybrid');
    setContactRules(location.contact_rules ?? []);
    setChannelTargets(location.channel_targets ?? []);
  }, [location]);

  useEffect(() => {
    setGpsOn(gpsEnabled ?? true);
  }, [gpsEnabled]);

  const toggleGps = async (checked: boolean) => {
    setError('');
    const previous = gpsOn;
    setGpsOn(checked); // optimistic
    try {
      await setGpsEnabled(checked);
    } catch (e) {
      setGpsOn(previous); // revert
      setError(friendlyErrorFrom(e));
    }
  };

  // A node transmits only to configured targets, so the switch on its own says
  // nothing about whether anything is being sent. Every affirmative bit of copy
  // below is derived from this, not from `enabled`.
  const targetCount = useMemo(
    () => contactRules.filter(r => r.enabled !== false).length + channelTargets.filter(c => c.enabled !== false).length,
    [contactRules, channelTargets],
  );
  const sharingActive = enabled && targetCount > 0;

  const preview = useMemo(() => {
    if (!enabled) {
      return 'Sharing is OFF. Your node will not publish periodic location packets.';
    }
    if (targetCount === 0) {
      return 'Sharing is ON but has no targets, so nothing is sent. Add a contact or a channel target below.';
    }

    return `Sharing ${tier === 'full' ? 'exact coordinates' : tier === 'coarse' ? 'coarse zone updates' : tier === 'presence' ? 'presence only' : 'off'} every ${interval}s using ${source}. Active targets: ${targetCount}.`;
  }, [enabled, tier, interval, source, targetCount]);

  const addContactRule = () => {
    setError('');
    const normalized = normalizeAddress(newContactAddress);
    if (normalized === null) {
      setError('Contact address must be 1-8 hex characters.');
      return;
    }
    if (contactRules.some(r => r.address === normalized)) {
      setError('Contact already exists.');
      return;
    }

    setContactRules([
      ...contactRules,
      { address: normalized, enabled: true, tier: 'coarse', interval_s: interval },
    ]);
    setNewContactAddress('');
  };

  const addChannelTarget = () => {
    setError('');
    if (channelTargets.some(c => c.channel === Number(newChannelTarget))) {
      setError('Channel target already exists.');
      return;
    }
    setChannelTargets([
      ...channelTargets,
      { channel: Number(newChannelTarget), enabled: true, tier: 'coarse', interval_s: interval },
    ]);
  };

  const savePolicy = async () => {
    setError('');
    setSaving(true);
    try {
      const sanitizedInterval = Math.max(30, Number(interval) || DEFAULT_INTERVAL);
      await setLocationConfig({
        enabled,
        default_tier: tier,
        tier,
        interval_s: sanitizedInterval,
        source,
        contact_rules: contactRules,
        channel_targets: channelTargets,
      });
    } catch (e) {
      setError(friendlyErrorFrom(e));
    } finally {
      setSaving(false);
    }
  };

  return (
    <div className={styles.section}>
      {error && <div className={styles.error}>{error}</div>}

      {gpsAvailable && (
        <div className={styles.row}>
          <label className={styles.toggle}>
            <input
              aria-label="GPS power"
              type="checkbox"
              checked={gpsOn}
              onChange={e => toggleGps(e.target.checked)}
            />
            <span>{gpsOn ? 'GPS on' : 'GPS off (sharing falls back to manual location)'}</span>
          </label>
        </div>
      )}

      <div className={styles.row}>
        <label className={styles.toggle}>
          <input
            aria-label="Enable location sharing"
            type="checkbox"
            checked={enabled}
            onChange={e => setEnabled(e.target.checked)}
          />
          <span>
            {!enabled
              ? 'Location sharing disabled'
              : sharingActive
                ? 'Location sharing enabled'
                : 'Location sharing enabled, no targets'}
          </span>
          {sharingActive ? <IconLocation size={14} /> : <IconLocationOff size={14} />}
        </label>
      </div>

      <div className={styles.row}>
        <label className={styles.label} htmlFor="location-tier">Default tier</label>
        <select id="location-tier" aria-label="Default tier" value={tier} onChange={e => setTier(e.target.value as LocationTier)}>
          {TIER_OPTIONS.map(t => <option key={t.value} value={t.value}>{t.label}</option>)}
        </select>
      </div>
      <div className={styles.dim} aria-label="Default tier description">{TIER_DESCRIPTIONS[tier]}</div>

      <div className={styles.row}>
        <label className={styles.label} htmlFor="location-interval">Interval (seconds)</label>
        <input
          id="location-interval"
          aria-label="Interval (seconds)"
          type="number"
          min={30}
          value={interval}
          onChange={e => setInterval(Number(e.target.value))}
        />
      </div>

      <div className={styles.row}>
        <label className={styles.label} htmlFor="location-source">Source</label>
        <select
          id="location-source"
          aria-label="Source"
          value={source}
          onChange={e => setSource(e.target.value as LocationSource)}
        >
          {SOURCE_OPTIONS.map(s => (
            <option key={s.value} value={s.value} disabled={!gpsAvailable && s.value !== 'manual'}>
              {s.label}
            </option>
          ))}
        </select>
      </div>
      {source === 'hybrid' && (
        <div className={styles.preview}>
          <strong>Hybrid explained:</strong> uses live GPS coordinates when available; if GPS is unavailable,
          Bramble falls back to your saved manual location so location sharing keeps working.
        </div>
      )}

      <div className={styles.block}>
        <div className={styles.blockHeader}><strong>Contact targets</strong></div>
        <div className={styles.addRow}>
          <input
            className={styles.addrInput}
            aria-label="Contact address (hex)"
            value={newContactAddress}
            onChange={e => setNewContactAddress(e.target.value)}
            placeholder="e.g. 1A2B3C4D"
          />
          <button className={styles.btnConfirm} onClick={addContactRule}>Add contact target</button>
        </div>
        {neighbors.length > 0 && (
          <div className={styles.quickAdd}>
            {neighbors.map(n => (
              <button className={styles.quickAddBtn} key={n.addr} onClick={() => setNewContactAddress(formatAddrHex(n.addr))}>
                {peerNames.get(n.addr) || formatAddrShort(n.addr)}
              </button>
            ))}
          </div>
        )}
        {contactRules.map((rule, idx) => {
          const name = resolveLabel(rule.address);
          return (
            <div key={rule.address} className={styles.contactCard}>
              <div className={styles.contactCardHeader}>
                <AddressLabel addr={parseInt(rule.address, 16)} name={name} />
                <label className={styles.inlineToggle}>
                  <input
                    type="checkbox"
                    checked={rule.enabled !== false}
                    onChange={e => {
                      const next = [...contactRules];
                      next[idx] = { ...next[idx], enabled: e.target.checked };
                      setContactRules(next);
                    }}
                  />
                </label>
              </div>
              <div className={styles.contactCardBody}>
                <label className={styles.contactCardField}>
                  <span className={styles.fieldLabel}>Tier</span>
                  <select
                    className={styles.tierSelect}
                    value={rule.tier}
                    onChange={e => {
                      const next = [...contactRules];
                      next[idx] = { ...next[idx], tier: e.target.value as LocationTier };
                      setContactRules(next);
                    }}
                  >
                    {TIER_OPTIONS.map(t => <option key={t.value} value={t.value}>{t.label}</option>)}
                  </select>
                </label>
                <label className={styles.contactCardField}>
                  <span className={styles.fieldLabel}>Interval</span>
                  <div className={styles.intervalWrap}>
                    <input
                      type="number"
                      min={30}
                      className={styles.intervalInput}
                      value={rule.interval_s}
                      onChange={e => {
                        const next = [...contactRules];
                        next[idx] = { ...next[idx], interval_s: Number(e.target.value) };
                        setContactRules(next);
                      }}
                    />
                    <span className={styles.fieldUnit}>sec</span>
                  </div>
                </label>
                <button className={styles.removeLink} onClick={() => setContactRules(contactRules.filter(r => r.address !== rule.address))}>Remove</button>
              </div>
            </div>
          );
        })}
      </div>

      <div className={styles.block}>
        <div className={styles.blockHeader}><strong>Channel targets</strong></div>
        <div className={styles.addRow}>
          <select aria-label="Channel target" value={newChannelTarget} onChange={e => setNewChannelTarget(Number(e.target.value))}>
            {channels.map(ch => (
              <option key={ch.index} value={ch.index}>#{ch.index} {ch.name || `channel-${ch.index}`}</option>
            ))}
          </select>
          <button className={styles.btnConfirm} onClick={addChannelTarget}>Add channel target</button>
        </div>

        {channelTargets.map((target, idx) => (
          <div key={target.channel} className={styles.contactRow}>
            <span>#{target.channel}</span>
            <label className={styles.inlineToggle}>
              <input
                type="checkbox"
                checked={target.enabled !== false}
                onChange={e => {
                  const next = [...channelTargets];
                  next[idx] = { ...next[idx], enabled: e.target.checked };
                  setChannelTargets(next);
                }}
              />
              Enabled
            </label>
            <select
              className={styles.tierSelect}
              value={target.tier}
              onChange={e => {
                const next = [...channelTargets];
                next[idx] = { ...next[idx], tier: e.target.value as LocationTier };
                setChannelTargets(next);
              }}
            >
              {TIER_OPTIONS.map(t => <option key={t.value} value={t.value}>{t.label}</option>)}
            </select>
            <input
              type="number"
              min={30}
              value={target.interval_s}
              onChange={e => {
                const next = [...channelTargets];
                next[idx] = { ...next[idx], interval_s: Number(e.target.value) };
                setChannelTargets(next);
              }}
            />
            <button className={styles.removeLink} onClick={() => setChannelTargets(channelTargets.filter(c => c.channel !== target.channel))}>Remove</button>
          </div>
        ))}
      </div>

      <div className={styles.preview} aria-label="Location policy preview">{preview}</div>

      <button className={styles.btnConfirm} onClick={savePolicy} disabled={saving}>{saving ? 'Saving…' : 'Save location policy'}</button>
    </div>
  );
}
