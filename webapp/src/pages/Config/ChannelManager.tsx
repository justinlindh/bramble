import { useState } from 'react';
import type { Channel } from '../../types/bramble';
import {
  addChannel,
  removeChannel,
  setDefaultChannel,
} from '../../store/actions';
import styles from './ChannelManager.module.css';

interface ChannelManagerProps {
  channels: Channel[];
}

export function ChannelManager({ channels }: ChannelManagerProps) {
  const [newName, setNewName] = useState('');
  const [newPsk, setNewPsk] = useState('');
  const [adding, setAdding] = useState(false);
  const [error, setError] = useState('');

  const handleAdd = async (e: React.FormEvent) => {
    e.preventDefault();
    const name = newName.trim();
    if (!name) return;
    setAdding(true);
    setError('');
    try {
      await addChannel(name, newPsk.trim() || undefined);
      setNewName('');
      setNewPsk('');
    } catch (err) {
      setError((err as Error).message);
    } finally {
      setAdding(false);
    }
  };

  const handleRemove = async (index: number, name: string) => {
    if (!confirm(`Remove channel "${name}"?`)) return;
    setError('');
    try {
      await removeChannel(index);
    } catch (err) {
      setError((err as Error).message);
    }
  };

  const handleSetDefault = async (index: number) => {
    setError('');
    try {
      await setDefaultChannel(index);
    } catch (err) {
      setError((err as Error).message);
    }
  };

  return (
    <div>
      {/* ── Channel list ── */}
      <div className={styles.list}>
        {channels.length === 0 && (
          <p style={{ color: 'var(--text-muted)', fontSize: '0.875rem' }}>
            No channels configured.
          </p>
        )}
        {channels.map((ch) => (
          <div key={ch.index} className={styles.channelRow}>
            <span className={styles.idx}>#{ch.index}</span>
            <span className={styles.chName}>{ch.name}</span>
            {ch.isDefault && (
              <span className={`${styles.badge} ${styles.badgeDefault}`}>
                default
              </span>
            )}
            {ch.hasPsk && (
              <span className={`${styles.badge} ${styles.badgePsk}`}>
                🔒 PSK
              </span>
            )}
            <span className={styles.epoch} title="Key rotation epoch">
              epoch {ch.epoch}
            </span>
            <div className={styles.rowActions}>
              {!ch.isDefault && (
                <button
                  className={styles.setDefaultBtn}
                  onClick={() => handleSetDefault(ch.index)}
                  title="Set as default channel"
                >
                  Set default
                </button>
              )}
              <button
                className={styles.removeBtn}
                onClick={() => handleRemove(ch.index, ch.name)}
                title={`Remove channel ${ch.name}`}
                disabled={ch.isDefault}
              >
                ✕
              </button>
            </div>
          </div>
        ))}
      </div>

      {/* ── Add channel form ── */}
      <form className={styles.addForm} onSubmit={handleAdd}>
        <input
          className={styles.addInput}
          type="text"
          placeholder="Channel name"
          value={newName}
          maxLength={16}
          onChange={(e) => setNewName(e.target.value)}
          aria-label="New channel name"
        />
        <input
          className={styles.pskInput}
          type="password"
          placeholder="PSK (optional)"
          value={newPsk}
          onChange={(e) => setNewPsk(e.target.value)}
          aria-label="Channel pre-shared key"
          autoComplete="new-password"
        />
        <button
          className={styles.addBtn}
          type="submit"
          disabled={adding || !newName.trim()}
        >
          {adding ? 'Adding…' : '+ Add Channel'}
        </button>
        {error && <span className={styles.error}>{error}</span>}
      </form>
    </div>
  );
}
