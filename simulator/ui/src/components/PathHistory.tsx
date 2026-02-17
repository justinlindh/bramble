import { useMemo, useState } from 'react';
import type { DeliveryRecord } from '../types';

interface PathHistoryProps {
  records: DeliveryRecord[];
}

interface PairSummary {
  key: string;
  from: string;
  to: string;
  count: number;
  lastPath: string[];
  paths: Map<string, number>; // path string -> count
  pathChanged: boolean;
}

function formatTimestamp(us: number): string {
  const ms = us / 1000;
  if (ms < 1000) return `${ms.toFixed(1)}ms`;
  return `${(ms / 1000).toFixed(3)}s`;
}

export function PathHistory({ records }: PathHistoryProps) {
  const [viewMode, setViewMode] = useState<'recent' | 'pairs'>('recent');

  const pairSummaries = useMemo(() => {
    const map = new Map<string, PairSummary>();
    for (const r of records) {
      const key = `${r.from}→${r.to}`;
      let summary = map.get(key);
      if (!summary) {
        summary = { key, from: r.from, to: r.to, count: 0, lastPath: [], paths: new Map(), pathChanged: false };
        map.set(key, summary);
      }
      const pathStr = r.path.join('→');
      summary.count++;
      const prevPath = summary.lastPath.join('→');
      if (prevPath && prevPath !== pathStr) {
        summary.pathChanged = true;
      }
      summary.lastPath = r.path;
      summary.paths.set(pathStr, (summary.paths.get(pathStr) ?? 0) + 1);
    }
    return Array.from(map.values());
  }, [records]);

  const recentRecords = useMemo(() => records.slice(-50).reverse(), [records]);

  return (
    <div style={{
      height: '100%',
      display: 'flex',
      flexDirection: 'column',
      background: '#0d1117',
      fontFamily: "'Fira Code', 'Consolas', monospace",
      fontSize: '11.5px',
    }}>
      {/* Header */}
      <div style={{
        padding: '6px 14px',
        borderBottom: '1px solid #21262d',
        display: 'flex',
        alignItems: 'center',
        gap: '12px',
        flexShrink: 0,
      }}>
        <span style={{ fontSize: '11px', color: '#8b949e', fontWeight: 600, textTransform: 'uppercase', letterSpacing: '0.08em' }}>
          Path History
        </span>
        <span style={{ fontSize: '11px', color: '#6e7681' }}>{records.length} deliveries</span>
        <div style={{ marginLeft: 'auto', display: 'flex', gap: '4px' }}>
          <TabBtn active={viewMode === 'recent'} onClick={() => setViewMode('recent')}>Recent</TabBtn>
          <TabBtn active={viewMode === 'pairs'} onClick={() => setViewMode('pairs')}>By Pair</TabBtn>
        </div>
      </div>

      {/* Content */}
      <div style={{ flex: 1, overflowY: 'auto', padding: '4px 0' }}>
        {records.length === 0 ? (
          <div style={{ color: '#6e7681', padding: '8px 14px' }}>
            No deliveries yet...
          </div>
        ) : viewMode === 'recent' ? (
          recentRecords.map(r => (
            <div key={r.id} style={{
              padding: '3px 14px',
              display: 'flex',
              gap: '8px',
              alignItems: 'baseline',
              borderLeft: '2px solid transparent',
            }}>
              <span style={{ color: '#6e7681', minWidth: '60px', flexShrink: 0 }}>
                [{formatTimestamp(r.timestamp_us)}]
              </span>
              <span style={{ color: '#3fb950', fontWeight: 600, minWidth: '100px', flexShrink: 0 }}>
                {r.from} → {r.to}
              </span>
              <span style={{ color: '#8b949e' }}>
                {r.path.join(' → ')}
              </span>
              <span style={{ color: '#58a6ff', flexShrink: 0 }}>
                {r.hopCount}h
              </span>
              {r.latencyMs != null && (
                <span style={{ color: '#d2a8ff', flexShrink: 0 }}>
                  {r.latencyMs.toFixed(1)}ms
                </span>
              )}
            </div>
          ))
        ) : (
          pairSummaries.map(s => (
            <div key={s.key} style={{
              padding: '6px 14px',
              borderBottom: '1px solid #21262d',
            }}>
              <div style={{ display: 'flex', gap: '8px', alignItems: 'center', marginBottom: '4px' }}>
                <span style={{ color: '#3fb950', fontWeight: 600 }}>{s.from} → {s.to}</span>
                <span style={{ color: '#6e7681' }}>×{s.count}</span>
                {s.pathChanged && (
                  <span style={{
                    fontSize: '10px',
                    color: '#f0883e',
                    background: 'rgba(240,136,62,0.15)',
                    padding: '1px 5px',
                    borderRadius: '3px',
                  }}>
                    route changed
                  </span>
                )}
              </div>
              {Array.from(s.paths.entries()).sort((a, b) => b[1] - a[1]).map(([path, count], i) => (
                <div key={i} style={{ color: '#8b949e', fontSize: '11px', paddingLeft: '8px' }}>
                  {path} <span style={{ color: '#6e7681' }}>×{count}</span>
                </div>
              ))}
            </div>
          ))
        )}
      </div>
    </div>
  );
}

function TabBtn({ active, onClick, children }: { active: boolean; onClick: () => void; children: React.ReactNode }) {
  return (
    <button
      onClick={onClick}
      style={{
        background: active ? '#21262d' : 'transparent',
        border: '1px solid ' + (active ? '#30363d' : 'transparent'),
        borderRadius: '4px',
        color: active ? '#e6edf3' : '#6e7681',
        fontSize: '11px',
        padding: '2px 8px',
        cursor: 'pointer',
        fontFamily: 'monospace',
      }}
    >
      {children}
    </button>
  );
}
