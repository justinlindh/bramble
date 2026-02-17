import { useEffect, useRef } from 'react';
import type { SimEvent } from '../types';

interface EventLogProps {
  events: SimEvent[];
}

const EVENT_COLORS: Record<string, string> = {
  anomaly:    '#f85149',
  dropped:    '#f0883e',
  route:      '#58a6ff',
  node_joined:'#3fb950',
  node_left:  '#8b949e',
  node_moved: '#d2a8ff',
  metrics:    '#6e7681',
  sim_ended:  '#f0883e',
};

function getColor(type: string): string {
  if (EVENT_COLORS[type]) return EVENT_COLORS[type];
  if (type.includes('anomaly')) return EVENT_COLORS.anomaly;
  if (type.includes('drop')) return EVENT_COLORS.dropped;
  if (type.includes('route')) return EVENT_COLORS.route;
  return '#6e7681';
}

function formatTimestamp(us: number): string {
  const ms = us / 1000;
  if (ms < 1000) return `${ms.toFixed(1)}ms`;
  return `${(ms / 1000).toFixed(3)}s`;
}

function formatDetails(details: Record<string, unknown>): string {
  const parts: string[] = [];
  for (const [k, v] of Object.entries(details)) {
    if (v === null || v === undefined) continue;
    if (typeof v === 'number') {
      parts.push(`${k}=${v % 1 === 0 ? v : v.toFixed(3)}`);
    } else {
      parts.push(`${k}=${JSON.stringify(v)}`);
    }
  }
  return parts.join(' ');
}

export function EventLog({ events }: EventLogProps) {
  const bottomRef = useRef<HTMLDivElement>(null);
  const containerRef = useRef<HTMLDivElement>(null);

  // Auto-scroll to bottom when new events arrive
  useEffect(() => {
    const container = containerRef.current;
    if (!container) return;
    const isNearBottom = container.scrollHeight - container.scrollTop - container.clientHeight < 60;
    if (isNearBottom) {
      bottomRef.current?.scrollIntoView({ behavior: 'smooth' });
    }
  }, [events]);

  return (
    <div style={{
      height: '200px',
      flexShrink: 0,
      background: '#0d1117',
      borderTop: '1px solid #21262d',
      display: 'flex',
      flexDirection: 'column',
    }}>
      {/* Header */}
      <div style={{
        padding: '6px 14px',
        borderBottom: '1px solid #21262d',
        display: 'flex',
        alignItems: 'center',
        justifyContent: 'space-between',
        flexShrink: 0,
      }}>
        <span style={{ fontSize: '11px', color: '#8b949e', fontWeight: 600, textTransform: 'uppercase', letterSpacing: '0.08em' }}>
          Event Log
        </span>
        <span style={{ fontSize: '11px', color: '#6e7681' }}>
          {events.length} events
        </span>
      </div>

      {/* Events */}
      <div
        ref={containerRef}
        style={{
          flex: 1,
          overflowY: 'auto',
          padding: '4px 0',
          fontFamily: "'Fira Code', 'Consolas', 'Courier New', monospace",
          fontSize: '11.5px',
          lineHeight: '1.5',
        }}
      >
        {events.length === 0 ? (
          <div style={{ color: '#6e7681', padding: '8px 14px' }}>
            Waiting for events...
          </div>
        ) : (
          events.map(event => {
            const color = getColor(event.type);
            const ts = formatTimestamp(event.timestamp_us);
            const details = formatDetails(event.details);
            return (
              <div
                key={event.id}
                style={{
                  padding: '1px 14px',
                  display: 'flex',
                  gap: '8px',
                  alignItems: 'baseline',
                  borderLeft: `2px solid transparent`,
                }}
              >
                <span style={{ color: '#6e7681', minWidth: '60px', flexShrink: 0 }}>
                  [{ts}]
                </span>
                <span style={{ color, fontWeight: 600, minWidth: '100px', flexShrink: 0 }}>
                  {event.type}
                </span>
                <span style={{ color: '#8b949e', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }}>
                  {details}
                </span>
              </div>
            );
          })
        )}
        <div ref={bottomRef} />
      </div>
    </div>
  );
}
