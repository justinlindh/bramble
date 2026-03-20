/**
 * PeerManager — lists known peers (union of neighbors + route destinations)
 * with client-side name assignment stored in localStorage.
 */
import { useRef, useState, useMemo } from 'react';
import type { Neighbor, Route } from '../../types/bramble';
import { useStore } from '../../store/index';
import { AddressLabel } from '../../components/AddressLabel';
import styles from './PeerManager.module.css';

// ─── localStorage helpers ─────────────────────────────────────────────────────

const LS_KEY = 'bramble:peerNames';
const LS_NOTES_KEY = 'bramble:peerNotes';

type ContactRecord = { name: string; note?: string };
type ContactsExport = {
  version: 1;
  exportedAt: string;
  contacts: Record<string, ContactRecord>;
};

function loadStringMap(key: string): Map<number, string> {
  try {
    const raw = localStorage.getItem(key);
    if (!raw) return new Map();
    const obj = JSON.parse(raw) as Record<string, string>;
    return new Map(Object.entries(obj).map(([k, v]) => [Number(k), String(v)]));
  } catch {
    return new Map();
  }
}

function saveStringMap(key: string, m: Map<number, string>): void {
  try {
    const obj: Record<string, string> = {};
    m.forEach((v, k) => {
      if (v) obj[k] = v;
    });
    localStorage.setItem(key, JSON.stringify(obj));
  } catch {
    // ignore quota errors
  }
}

function loadNames(): Map<number, string> {
  return loadStringMap(LS_KEY);
}

function saveNames(m: Map<number, string>): void {
  saveStringMap(LS_KEY, m);
}

function loadNotes(): Map<number, string> {
  return loadStringMap(LS_NOTES_KEY);
}

function saveNotes(m: Map<number, string>): void {
  saveStringMap(LS_NOTES_KEY, m);
}

function toHex(addr: number): string {
  return addr.toString(16).toUpperCase().padStart(8, '0');
}

function formatAgo(ms: number): string {
  if (ms < 1000) return 'just now';
  const s = Math.floor(ms / 1000);
  if (s < 60) return `${s}s ago`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m ago`;
  return `${Math.floor(m / 60)}h ago`;
}

async function readFileText(file: File): Promise<string> {
  if (typeof file.text === 'function') {
    return file.text();
  }

  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result ?? ''));
    reader.onerror = () => reject(new Error('read failed'));
    reader.readAsText(file);
  });
}

function parseImportedContacts(raw: unknown): Map<number, ContactRecord> {
  if (!raw || typeof raw !== 'object') return new Map();

  const contactsObj =
    raw && typeof raw === 'object' && 'contacts' in raw && raw.contacts && typeof raw.contacts === 'object'
      ? (raw.contacts as Record<string, unknown>)
      : (raw as Record<string, unknown>);

  const out = new Map<number, ContactRecord>();

  for (const [key, value] of Object.entries(contactsObj)) {
    const normalized = key.trim().replace(/^0x/i, '');
    const addr = parseInt(normalized, 16);
    if (Number.isNaN(addr) || addr < 0 || addr > 0xffffffff) continue;

    if (typeof value === 'string') {
      const name = value.trim();
      if (name) out.set(addr, { name });
      continue;
    }

    if (!value || typeof value !== 'object') continue;
    const rec = value as Record<string, unknown>;
    const name = typeof rec.name === 'string' ? rec.name.trim() : '';
    if (!name) continue;

    const note = typeof rec.note === 'string' ? rec.note.trim() : undefined;
    out.set(addr, note ? { name, note } : { name });
  }

  return out;
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
  note: string | undefined;
  onSaveName: (addr: number, name: string) => void;
  onSaveNote: (addr: number, note: string) => void;
}

function PeerRow({ peer, name, note, onSaveName, onSaveNote }: PeerRowProps) {
  const [editing, setEditing] = useState(false);
  const [draft, setDraft] = useState(name ?? '');
  const [draftNote, setDraftNote] = useState(note ?? '');

  const handleSave = (e: React.FormEvent) => {
    e.preventDefault();
    onSaveName(peer.addr, draft.trim());
    onSaveNote(peer.addr, draftNote.trim());
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
          <textarea
            className={styles.notesInput}
            value={draftNote}
            maxLength={256}
            placeholder="Notes (optional)"
            onChange={(e) => setDraftNote(e.target.value)}
            aria-label="Peer notes"
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
          {note && <span className={styles.peerNote}>{note}</span>}
          {peer.lastHeardMs !== undefined && (
            <span className={styles.lastHeard}>{formatAgo(peer.lastHeardMs)}</span>
          )}
          <button
            className={styles.editBtn}
            onClick={() => {
              setDraft(name ?? '');
              setDraftNote(note ?? '');
              setEditing(true);
            }}
            title="Edit peer details"
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
  const [notes, setNotes] = useState<Map<number, string>>(loadNotes);
  const [addAddr, setAddAddr] = useState('');
  const [addName, setAddName] = useState('');
  const [addError, setAddError] = useState('');
  const [addSuccess, setAddSuccess] = useState('');
  const [importStatus, setImportStatus] = useState('');
  const fileInputRef = useRef<HTMLInputElement | null>(null);

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
    // Sync to Zustand store so Chat/Map reflect the name immediately (BUG-09)
    useStore.getState().setPeerName(addr, name);
  };

  const handleSaveNote = (addr: number, note: string) => {
    setNotes((prev) => {
      const next = new Map(prev);
      if (note) {
        next.set(addr, note);
      } else {
        next.delete(addr);
      }
      saveNotes(next);
      return next;
    });
  };

  const handleAddContact = (e: React.FormEvent) => {
    e.preventDefault();
    setAddError('');
    setAddSuccess('');
    const raw = addAddr.trim().replace(/^0x/i, '');
    const addr = parseInt(raw, 16);
    if (isNaN(addr) || addr < 0 || addr > 0xffffffff) {
      setAddError('Invalid address (use hex, e.g. 0xDEADBEEF)');
      return;
    }
    if (addName.trim()) {
      handleSaveName(addr, addName.trim());
    }
    const hex = `0x${addr.toString(16).toUpperCase().padStart(8, '0')}`;
    const label = addName.trim() || hex;
    setAddSuccess(`Name saved for ${label} — will appear in chat when discovered on mesh.`);
    setAddAddr('');
    setAddName('');
    setTimeout(() => setAddSuccess(''), 5000);
  };

  const handleExport = () => {
    const contacts: Record<string, ContactRecord> = {};
    names.forEach((name, addr) => {
      if (!name) return;
      const note = notes.get(addr)?.trim();
      contacts[toHex(addr)] = note ? { name, note } : { name };
    });

    const payload: ContactsExport = {
      version: 1,
      exportedAt: new Date().toISOString(),
      contacts,
    };

    try {
      const blob = new Blob([JSON.stringify(payload, null, 2)], { type: 'application/json' });
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      const dateTag = new Date().toISOString().slice(0, 10);
      a.href = url;
      a.download = `bramble-contacts-${dateTag}.json`;
      document.body.appendChild(a);
      a.click();
      a.remove();
      URL.revokeObjectURL(url);
      setImportStatus('Contacts exported.');
    } catch {
      setImportStatus('Could not export contacts.');
    }
  };

  const handleImportClick = () => {
    setImportStatus('');
    fileInputRef.current?.click();
  };

  const handleImportFile: React.ChangeEventHandler<HTMLInputElement> = async (e) => {
    const file = e.target.files?.[0];
    if (!file) return;

    try {
      const text = await readFileText(file);
      const parsed = JSON.parse(text) as unknown;
      const imported = parseImportedContacts(parsed);
      if (imported.size === 0) {
        setImportStatus('No valid contacts found in file.');
        return;
      }

      const conflictCount = Array.from(imported.entries()).filter(([addr, record]) => {
        const existing = names.get(addr);
        return !!existing && existing !== record.name;
      }).length;

      const overwriteExisting = conflictCount > 0
        ? window.confirm(`${conflictCount} contact(s) already have names. Click OK to overwrite existing names, or Cancel to keep existing names.`)
        : false;

      const nextNames = new Map(names);
      const nextNotes = new Map(notes);
      let importedCount = 0;

      imported.forEach((record, addr) => {
        const existing = nextNames.get(addr);
        const canWrite = !existing || overwriteExisting;

        if (canWrite) {
          nextNames.set(addr, record.name);
          importedCount += 1;
        }

        if (record.note) {
          const existingNote = nextNotes.get(addr);
          if (!existingNote || overwriteExisting) {
            nextNotes.set(addr, record.note);
          }
        }
      });

      setNames(nextNames);
      setNotes(nextNotes);
      saveNames(nextNames);
      saveNotes(nextNotes);
      setImportStatus(`Imported ${importedCount} contact(s).`);
    } catch {
      setImportStatus('Could not import contacts file.');
    } finally {
      e.target.value = '';
    }
  };

  return (
    <div>
      <div className={styles.toolsRow}>
        <div className={styles.addTitle}>Contacts backup</div>
        <div className={styles.toolsActions}>
          <button className={styles.secondaryBtn} type="button" onClick={handleExport}>
            Export Contacts
          </button>
          <button className={styles.secondaryBtn} type="button" onClick={handleImportClick}>
            Import Contacts
          </button>
          <input
            ref={fileInputRef}
            className={styles.hiddenInput}
            type="file"
            accept="application/json,.json"
            onChange={handleImportFile}
            aria-label="Import contacts file"
          />
        </div>
        {importStatus && <div className={styles.importStatus}>{importStatus}</div>}
      </div>

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
              note={notes.get(p.addr)}
              onSaveName={handleSaveName}
              onSaveNote={handleSaveNote}
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
          {addSuccess && <span className={styles.success}>{addSuccess}</span>}
        </form>
      </div>
    </div>
  );
}
