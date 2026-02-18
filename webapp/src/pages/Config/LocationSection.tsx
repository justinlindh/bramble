import { useState } from 'react';
import type { LocationConfig, LocationTier, LocationContact } from '../../types/bramble';
import type { Neighbor } from '../../types/bramble';
import { setLocationConfig, setLocationContact, removeLocationContact } from '../../store/actions';
import { IconLocation, IconLocationOff, IconWarning } from '../../components/Icons';
import styles from './LocationSection.module.css';

interface LocationSectionProps {
  location: LocationConfig;
  neighbors: Neighbor[];
  gpsAvailable?: boolean;
}

const TIER_LABELS: Record<LocationTier, { label: string; desc: string; cls: string }> = {
  off:      { label: 'Off',      desc: 'No location shared',    cls: 'tierOff' },
  presence: { label: 'Presence', desc: 'Online status only',    cls: 'tierPresence' },
  coarse:   { label: 'Zone',     desc: '~1km grid square',      cls: 'tierCoarse' },
  full:     { label: 'Exact',    desc: 'Full GPS coordinates',  cls: 'tierFull' },
};

function shortAddr(addr: number): string {
  return '0x' + addr.toString(16).toUpperCase().padStart(8, '0').slice(-4);
}

export function LocationSection({ location, neighbors, gpsAvailable = false }: LocationSectionProps) {
  const [error, setError] = useState('');
  const [showWarning, setShowWarning] = useState(false);
  const [addingPeer, setAddingPeer] = useState(false);
  const [newPeerAddr, setNewPeerAddr] = useState('');
  const [newPeerTier, setNewPeerTier] = useState<LocationTier>('coarse');

  const handleGpsToggle = async (enabled: boolean) => {
    if (enabled && !location.enabled) {
      setShowWarning(true);
      return;
    }
    try {
      await setLocationConfig({ enabled });
    } catch (e) {
      setError((e as Error).message);
    }
  };

  const confirmEnable = async () => {
    setShowWarning(false);
    try {
      await setLocationConfig({ enabled: true });
    } catch (e) {
      setError((e as Error).message);
    }
  };

  const handleAddContact = async () => {
    const addr = parseInt(newPeerAddr.replace(/^0x/i, ''), 16);
    if (isNaN(addr) || addr === 0) {
      setError('Invalid address');
      return;
    }
    try {
      await setLocationContact(addr, newPeerTier);
      setAddingPeer(false);
      setNewPeerAddr('');
    } catch (e) {
      setError((e as Error).message);
    }
  };

  const handleChangeTier = async (c: LocationContact, tier: LocationTier) => {
    try {
      await setLocationContact(c.addr, tier);
    } catch (e) {
      setError((e as Error).message);
    }
  };

  const handleRemoveContact = async (addr: number) => {
    try {
      await removeLocationContact(addr);
    } catch (e) {
      setError((e as Error).message);
    }
  };

  const sharedWith = location.contacts.filter(c => c.tier !== 'off');

  return (
    <div className={styles.section}>
      {error && <div className={styles.error}>{error}</div>}

      {/* Warning dialog */}
      {showWarning && (
        <div className={styles.warning}>
          <IconWarning size={16} />
          <div>
            <strong>Enable GPS?</strong>
            <p>Your position will be available to share with selected peers.
               No location is shared until you add contacts below.</p>
          </div>
          <div className={styles.warningActions}>
            <button onClick={confirmEnable} className={styles.btnConfirm}>Enable</button>
            <button onClick={() => setShowWarning(false)} className={styles.btnCancel}>Cancel</button>
          </div>
        </div>
      )}

      {/* GPS toggle */}
      <div className={styles.row}>
        <span className={styles.label}>GPS</span>
        {gpsAvailable ? (
          <label className={styles.toggle}
                 title={location.enabled
                   ? 'GPS is active — location can be shared with contacts'
                   : 'GPS is off — no location data collected or shared'}>
            <input
              type="checkbox"
              checked={location.enabled}
              onChange={(e) => handleGpsToggle(e.target.checked)}
            />
            <span>{location.enabled ? 'Enabled' : 'Disabled'}</span>
            {location.enabled
              ? <IconLocation size={14} />
              : <IconLocationOff size={14} />}
          </label>
        ) : (
          <span className={styles.unavailable} title="This device has no GPS hardware. Boards with GPS (e.g. T-Beam) will show controls here.">
            No GPS hardware
            <IconLocationOff size={14} />
          </span>
        )}
      </div>

      {/* Sharing summary */}
      {location.enabled && (
        <div className={styles.row}>
          <span className={styles.label}>Sharing with</span>
          <span className={sharedWith.length > 0 ? styles.shareActive : styles.shareNone}>
            {sharedWith.length === 0
              ? 'Nobody — add contacts below'
              : `${sharedWith.length} peer${sharedWith.length > 1 ? 's' : ''}`}
          </span>
        </div>
      )}

      {/* Contact list */}
      {location.enabled && (
        <div className={styles.contacts}>
          <div className={styles.contactsHeader}>
            <strong>Location Contacts</strong>
            <button className={styles.btnSmall} onClick={() => setAddingPeer(true)}>+ Add</button>
          </div>

          {/* Add contact form */}
          {addingPeer && (
            <div className={styles.addForm}>
              <select value={newPeerTier} onChange={e => setNewPeerTier(e.target.value as LocationTier)}>
                <option value="presence">Presence (online only)</option>
                <option value="coarse">Zone (~1km)</option>
                <option value="full">Exact (full GPS)</option>
              </select>
              {/* Show neighbors as quick-add buttons */}
              {neighbors.length > 0 && (
                <div className={styles.quickAdd}>
                  {neighbors.map(n => (
                    <button key={n.addr} className={styles.quickAddBtn}
                      onClick={() => setNewPeerAddr(n.addr.toString(16))}>
                      {shortAddr(n.addr)}
                    </button>
                  ))}
                </div>
              )}
              <div className={styles.addRow}>
                <input
                  type="text"
                  value={newPeerAddr}
                  onChange={e => setNewPeerAddr(e.target.value)}
                  placeholder="Address (hex)"
                  className={styles.addrInput}
                />
                <button className={styles.btnConfirm} onClick={handleAddContact}>Add</button>
                <button className={styles.btnCancel} onClick={() => setAddingPeer(false)}>Cancel</button>
              </div>
            </div>
          )}

          {/* Contact rows */}
          {location.contacts.length === 0 && !addingPeer && (
            <p className={styles.empty}>No location contacts. Add peers to share your position with.</p>
          )}
          {location.contacts.map(c => (
            <div key={c.addr} className={styles.contactRow}>
              <span className={styles.mono}>{shortAddr(c.addr)}</span>
              <select
                value={c.tier}
                onChange={e => handleChangeTier(c, e.target.value as LocationTier)}
                className={`${styles.tierSelect} ${styles[TIER_LABELS[c.tier].cls]}`}
              >
                <option value="off">Off</option>
                <option value="presence">Presence</option>
                <option value="coarse">Zone (~1km)</option>
                <option value="full">Exact</option>
              </select>
              <button
                className={styles.removeBtn}
                onClick={() => handleRemoveContact(c.addr)}
                title="Remove contact"
              >✕</button>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
