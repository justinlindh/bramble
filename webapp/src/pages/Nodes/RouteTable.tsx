import { useState } from 'react';
import type { Route } from '../../types/bramble';
import { AddressLabel } from '../../components/AddressLabel';
import styles from './RouteTable.module.css';

// ─── Types ────────────────────────────────────────────────────────────────────

type SortKey = 'dest' | 'nextHop' | 'hopCount' | 'metric' | 'state' | 'lastUsedMs';
type SortDir = 'asc' | 'desc';

// ─── Helpers ──────────────────────────────────────────────────────────────────

function formatAgo(ms: number): string {
  if (ms < 1000) return 'just now';
  const s = Math.floor(ms / 1000);
  if (s < 60) return `${s}s ago`;
  const m = Math.floor(s / 60);
  if (m < 60) return `${m}m ago`;
  return `${Math.floor(m / 60)}h ago`;
}

// ─── State badge ─────────────────────────────────────────────────────────────

function StateBadge({ state }: { state: Route['state'] }) {
  const cls = {
    active: styles.stateActive,
    stale: styles.stateStale,
    broken: styles.stateBroken,
    discovering: styles.stateDiscovering,
  }[state];

  return (
    <span className={`${styles.stateBadge} ${cls}`}>
      {state === 'discovering' && <span className={styles.spinner} aria-hidden />}
      {state}
    </span>
  );
}

// ─── Sort comparator ─────────────────────────────────────────────────────────

function compareRoutes(a: Route, b: Route, key: SortKey, dir: SortDir): number {
  let cmp = 0;
  if (key === 'dest') cmp = a.dest - b.dest;
  else if (key === 'nextHop') cmp = a.nextHop - b.nextHop;
  else if (key === 'hopCount') cmp = a.hopCount - b.hopCount;
  else if (key === 'metric') cmp = a.metric - b.metric;
  else if (key === 'state') cmp = a.state.localeCompare(b.state);
  else if (key === 'lastUsedMs') cmp = a.lastUsedMs - b.lastUsedMs;
  return dir === 'asc' ? cmp : -cmp;
}

// ─── Column header ────────────────────────────────────────────────────────────

interface ThProps {
  label: string;
  sortKey: SortKey;
  current: SortKey;
  dir: SortDir;
  onSort: (k: SortKey) => void;
  title?: string;
}

function Th({ label, sortKey, current, dir, onSort, title }: ThProps) {
  const active = current === sortKey;
  return (
    <th onClick={() => onSort(sortKey)} title={title}>
      {label}
      <span className={styles.sortIcon}>
        {active ? (dir === 'asc' ? '▲' : '▼') : '⇅'}
      </span>
    </th>
  );
}

// ─── Main component ───────────────────────────────────────────────────────────

interface RouteTableProps {
  routes: Route[];
}

export function RouteTable({ routes }: RouteTableProps) {
  const [sortKey, setSortKey] = useState<SortKey>('dest');
  const [sortDir, setSortDir] = useState<SortDir>('asc');

  const handleSort = (key: SortKey) => {
    if (key === sortKey) {
      setSortDir((d) => (d === 'asc' ? 'desc' : 'asc'));
    } else {
      setSortKey(key);
      setSortDir('asc');
    }
  };

  const sorted = [...routes].sort((a, b) => compareRoutes(a, b, sortKey, sortDir));

  return (
    <div className={styles.wrapper}>
      <table className={styles.table} aria-label="Routing table">
        <thead>
          <tr>
            <Th label="Destination" title="The node this route leads to." sortKey="dest" current={sortKey} dir={sortDir} onSort={handleSort} />
            <Th label="Next Hop" title="The neighbor node that will relay messages toward the destination." sortKey="nextHop" current={sortKey} dir={sortDir} onSort={handleSort} />
            <Th label="Hops" title="Number of relay nodes between you and the destination. Fewer hops = faster delivery." sortKey="hopCount" current={sortKey} dir={sortDir} onSort={handleSort} />
            <Th label="Metric" title="Route quality score (0–255, lower is better). Combines delivery rate, airtime usage, and latency." sortKey="metric" current={sortKey} dir={sortDir} onSort={handleSort} />
            <Th label="State" title="Route status: Active (in use), Stale (not recently confirmed), or Broken (failed delivery)." sortKey="state" current={sortKey} dir={sortDir} onSort={handleSort} />
            <Th label="Age" title="Time since this route was last used or confirmed." sortKey="lastUsedMs" current={sortKey} dir={sortDir} onSort={handleSort} />
          </tr>
        </thead>
        <tbody>
          {sorted.length === 0 ? (
            <tr className={styles.emptyRow}>
              <td colSpan={6}>No routes in table.</td>
            </tr>
          ) : (
            sorted.map((r) => (
              <tr key={`${r.dest}-${r.nextHop}`}>
                <td><AddressLabel addr={r.dest} short /></td>
                <td><AddressLabel addr={r.nextHop} short /></td>
                <td className={styles.mono}>{r.hopCount}</td>
                <td className={styles.mono}>{r.metric}</td>
                <td><StateBadge state={r.state} /></td>
                <td className={styles.mono}>{formatAgo(r.lastUsedMs)}</td>
              </tr>
            ))
          )}
        </tbody>
      </table>
    </div>
  );
}
