/**
 * PeerManager — lists known peers (union of neighbors + route destinations)
 * with client-side name assignment stored in localStorage.
 */
import { useState, useMemo } from 'react';
import type { Neighbor, Route } from '../../types/bramble';
import { AddressLabel } from '../../components/AddressLabel';
import styles from './PeerManager.module.css';

// ─── localStorage helpers ─────────────────────────────────────────────────────

const LS_KEY = 'bramble:peerNames';

function loadNames(): Map<number, string> {
  try {
    const raw = localStorage.getItem(LS_KEY);
    if (!raw) return new Map();
    const obj = JSON.parse(raw) as Record<string, string>;
    return new Map(Object.entries(obj).map(([k, v]) => [Number(k), v]));
  } catch {
    return new Map();
  }
}

function saveNames(m: Map<number, string>): void {
  try {
    const obj: Record<string, string> = {};
    m.forEach((v, k) => (obj[k] = v));
    localStorage.setItem(LS_KEY, JSON.stringify(obj));
  } catch {
    // ignore quota errors
  }
}

function formatAgo(ms: number): string {
  if (ms < 1000) return 'just now';
  const s = Math.floor(ms / 1000);
  if (s < 60) return `${s}s ago`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m ago`;
  return `${Math.floor(m / 60)}h ago`;
}

// ─── Types ────────────────────────────────────────────────────────────────────

interface PeerEntry {
  addr: number;
  lastHeardMs?: number;
}

// ─── Single peer row ─────────────────────────────────────────────────────────

interface PeerRowProps {
  peer: PeerEntry;
  name: string | undefined;
  onSaveName: (addr: number, name: string) => void;
}

function PeerRow({ peer, name, onSaveName }: PeerRowProps) {
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState(name ?? '');

  const handleSave = (e: React.FormEvent) => {
    e.preventDefault();
    onSaveName(peer.addr, draft.trim());
    setEditing(false);
  };

  return (
    <div className={styles.peerRow}>
      <AddressLabel addr={peer.addr} short className={styles.addr} />
      {editing ? (
        <form className={styles.editForm} onSubmit={handleSave}>
          <input
            className={styles.nameInput}
            type="text"
            value={draft}
            autoFocus
            maxLength={32}
            placeholder="Enter name"
            onChange={(e) => setDraft(e.target.value)}
            aria-label="Peer name"
          />
          <button className={styles.saveNameBtn} type="submit">
            ✓
          </button>
          <button
            className={styles.editBtn}
            type="button"
            onClick={() => setEditing(false)}
          >
            ✕
          </button>
        </form>
      ) : (
        <>
          {name ? (
            <span className={styles.peerName}>{name}</span>
          ) : (
            <span className={styles.noName}>—</span>
          )}
          {peer.lastHeardMs !== undefined && (
            <span className={styles.lastHeard}>{formatAgo(peer.lastHeardMs)}</span>
          )}
          <button
            className={styles.editBtn}
            onClick={() => {
              setDraft(name ?? '');
              setEditing(true);
            }}
            title="Edit name"
          >
            ✏ Edit
          </button>
        </>
      )}
    </div>
  );
}

// ─── Main component ───────────────────────────────────────────────────────────

interface PeerManagerProps {
  neighbors: Neighbor[];
  routes: Route[];
}

export function PeerManager({ neighbors, routes }: PeerManagerProps) {
  const [names, setNames] = useState<Map<number, string>>(loadNames);
  const [addAddr, setAddAddr] = useState('');
  const [addName, setAddName] = useState('');
  const [addError, setAddError] = useState('');

  // Build deduplicated peer list from neighbors + route destinations
  const peers = useMemo((): PeerEntry[] => {
    const map = new Map<number, PeerEntry>();

    for (const n of neighbors) {
      map.set(n.addr, { addr: n.addr, lastHeardMs: n.lastHeardMs });
    }
    for (const r of routes) {
      if (!map.has(r.dest)) {
        map.set(r.dest, { addr: r.dest });
      }
    }

    return Array.from(map.values()).sort((a, b) => a.addr - b.addr);
  }, [neighbors, routes]);

  const handleSaveName = (addr: number, name: string) => {
    setNames((prev) => {
      const next = new Map(prev);
      if (name) {
        next.set(addr, name);
      } else {
        next.delete(addr);
      }
      saveNames(next);
      return next;
    });
  };

  const handleAddContact = (e: React.FormEvent) => {
    e.preventDefault();
    setAddError('');
    const raw = addAddr.trim().replace(/^0x/i, '');
    const addr = parseInt(raw, 16);
    if (isNaN(addr) || addr < 0 || addr > 0xffffffff) {
      setAddError('Invalid address (use hex, e.g. 0xDEADBEEF)');
      return;
    }
    if (addName.trim()) {
      handleSaveName(addr, addName.trim());
    }
    setAddAddr('');
    setAddName('');
  };

  return (
    <div>
      {/* ── Peer list ── */}
      <div className={styles.peerList}>
        {peers.length === 0 ? (
          <p className={styles.emptyHint}>
            No peers discovered yet. Connect to a node and let it run for a bit.
          </p>
        ) : (
          peers.map((p) => (
            <PeerRow
              key={p.addr}
              peer={p}
              name={names.get(p.addr)}
              onSaveName={handleSaveName}
            />
          ))
        )}
      </div>

      {/* ── Add contact ── */}
      <div className={styles.addSection}>
        <div className={styles.addTitle}>Add contact</div>
        <form className={styles.addForm} onSubmit={handleAddContact}>
          <input
            className={styles.addrInput}
            type="text"
            placeholder="0xDEADBEEF"
            value={addAddr}
            onChange={(e) => setAddAddr(e.target.value)}
            aria-label="Peer address"
          />
          <input
            className={styles.contactNameInput}
            type="text"
            placeholder="Name (optional)"
            value={addName}
            maxLength={32}
            onChange={(e) => setAddName(e.target.value)}
            aria-label="Contact name"
          />
          <button
            className={styles.addBtn}
            type="submit"
            disabled={!addAddr.trim()}
          >
            + Add
          </button>
          {addError && <span className={styles.error}>{addError}</span>}
        </form>
      </div>
    </div>
  );
}
