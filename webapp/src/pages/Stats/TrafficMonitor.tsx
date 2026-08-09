import { useEffect, useMemo, useState } from 'react';
import { useStore } from '../../store/index';
import { loadTrafficDebugStatus, loadTrafficEvents } from '../../store/actions';
import { usePoll } from '../../hooks/usePoll';
import type { TrafficEvent, TrafficCategory, TrafficDirection, AirtimeBucket } from '../../types/bramble';
import styles from './TrafficMonitor.module.css';

interface RollingMetrics {
  totalAirtimeUs: number;
  categoryBreakdown: Record<TrafficCategory, number>;
  bucketBreakdown: Record<AirtimeBucket, number>;
  eventCount: number;
}

const TRAFFIC_FILTERS_KEY = 'bramble:traffic-monitor:filters';

type TrafficFilterState = {
  category: TrafficCategory | 'all';
  direction: TrafficDirection | 'all';
  bucket: AirtimeBucket | 'all';
};

const CATEGORY_FILTER_VALUES: TrafficFilterState['category'][] = ['all', 'beacon', 'timesync', 'routing', 'ack', 'chat', 'maintenance', 'other'];
const DIRECTION_FILTER_VALUES: TrafficFilterState['direction'][] = ['all', 'tx', 'rx'];
const BUCKET_FILTER_VALUES: TrafficFilterState['bucket'][] = ['all', 'broadcast', 'normal', 'critical'];

function isValidFilterValue<T extends string>(value: unknown, allowed: readonly T[]): value is T {
  return typeof value === 'string' && (allowed as readonly string[]).includes(value);
}

function loadPersistedFilters(): TrafficFilterState {
  const fallback: TrafficFilterState = { category: 'all', direction: 'all', bucket: 'all' };
  try {
    const raw = localStorage.getItem(TRAFFIC_FILTERS_KEY);
    if (!raw) return fallback;
    const parsed = JSON.parse(raw) as Partial<TrafficFilterState>;
    return {
      category: isValidFilterValue(parsed.category, CATEGORY_FILTER_VALUES) ? parsed.category : 'all',
      direction: isValidFilterValue(parsed.direction, DIRECTION_FILTER_VALUES) ? parsed.direction : 'all',
      bucket: isValidFilterValue(parsed.bucket, BUCKET_FILTER_VALUES) ? parsed.bucket : 'all',
    };
  } catch {
    return fallback;
  }
}

function savePersistedFilters(filters: TrafficFilterState): void {
  try {
    localStorage.setItem(TRAFFIC_FILTERS_KEY, JSON.stringify(filters));
  } catch {
    // noop
  }
}

function computeMetrics(events: TrafficEvent[], windowMs: number): RollingMetrics {
  const now = Date.now();
  const cutoff = now - windowMs;
  const filtered = events.filter((e) => e.timestampMs >= cutoff);

  const totalAirtimeUs = filtered.reduce((sum, e) => sum + e.airtimeDebitUs, 0);
  const categoryBreakdown: Record<TrafficCategory, number> = {
    beacon: 0,
    timesync: 0,
    routing: 0,
    ack: 0,
    chat: 0,
    maintenance: 0,
    other: 0,
  };
  const bucketBreakdown: Record<AirtimeBucket, number> = {
    broadcast: 0,
    normal: 0,
    critical: 0,
  };

  for (const e of filtered) {
    categoryBreakdown[e.category] = (categoryBreakdown[e.category] || 0) + e.airtimeDebitUs;
    bucketBreakdown[e.airtimeBucket] = (bucketBreakdown[e.airtimeBucket] || 0) + e.airtimeDebitUs;
  }

  return { totalAirtimeUs, categoryBreakdown, bucketBreakdown, eventCount: filtered.length };
}

function formatAirtime(us: number): string {
  if (us < 1000) return `${us}μs`;
  if (us < 1_000_000) return `${(us / 1000).toFixed(1)}ms`;
  return `${(us / 1_000_000).toFixed(2)}s`;
}

function MetricsWindow({ label, metrics }: { label: string; metrics: RollingMetrics }) {
  const categoryRows = Object.entries(metrics.categoryBreakdown)
    .filter(([_, us]) => us > 0)
    .sort(([_, a], [__, b]) => b - a);

  const bucketRows = Object.entries(metrics.bucketBreakdown)
    .filter(([_, us]) => us > 0)
    .sort(([_, a], [__, b]) => b - a);

  return (
    <div className={styles.windowCard}>
      <div className={styles.windowTitle}>{label}</div>
      {metrics.totalAirtimeUs === 0 ? (
        <div className={styles.empty}>No events in this window.</div>
      ) : (
        <>
          <div className={styles.windowMeta}>
            {formatAirtime(metrics.totalAirtimeUs)} · {metrics.eventCount} events
          </div>

          <div className={styles.blockTitle}>Category</div>
          {categoryRows.map(([name, us]) => (
            <div key={`cat-${label}-${name}`} className={styles.metricRow}>
              <span className={styles.metricName}>{name}</span>
              <span className={`${styles.metricVal} ${styles.mono}`}>{formatAirtime(us)}</span>
              <span className={`${styles.metricPct} ${styles.mono}`}>{((100 * us) / metrics.totalAirtimeUs).toFixed(1)}%</span>
            </div>
          ))}

          <div className={styles.blockTitle}>Bucket</div>
          {bucketRows.map(([name, us]) => (
            <div key={`bucket-${label}-${name}`} className={styles.metricRow}>
              <span className={styles.metricName}>{name}</span>
              <span className={`${styles.metricVal} ${styles.mono}`}>{formatAirtime(us)}</span>
              <span className={`${styles.metricPct} ${styles.mono}`}>{((100 * us) / metrics.totalAirtimeUs).toFixed(1)}%</span>
            </div>
          ))}
        </>
      )}
    </div>
  );
}

export function TrafficMonitor() {
  const trafficDebugStatus = useStore((s) => s.trafficDebugStatus);
  const trafficEvents = useStore((s) => s.trafficEvents);
  const isConnected = useStore((s) => s.connectionState === 'connected');

  const persistedFilters = useMemo(() => loadPersistedFilters(), []);
  const [categoryFilter, setCategoryFilter] = useState<TrafficCategory | 'all'>(persistedFilters.category);
  const [directionFilter, setDirectionFilter] = useState<TrafficDirection | 'all'>(persistedFilters.direction);
  const [bucketFilter, setBucketFilter] = useState<AirtimeBucket | 'all'>(persistedFilters.bucket);

  usePoll(loadTrafficDebugStatus, 5000, { enabled: isConnected });

  usePoll(
    () => {
      const highestSeq = trafficEvents.length > 0 ? trafficEvents[trafficEvents.length - 1].seq : undefined;
      return loadTrafficEvents(highestSeq);
    },
    2000,
    { enabled: isConnected },
  );

  useEffect(() => {
    savePersistedFilters({
      category: categoryFilter,
      direction: directionFilter,
      bucket: bucketFilter,
    });
  }, [categoryFilter, directionFilter, bucketFilter]);

  const filteredEvents = useMemo(
    () =>
      trafficEvents.filter((e) => {
        if (categoryFilter !== 'all' && e.category !== categoryFilter) return false;
        if (directionFilter !== 'all' && e.direction !== directionFilter) return false;
        if (bucketFilter !== 'all' && e.airtimeBucket !== bucketFilter) return false;
        return true;
      }),
    [trafficEvents, categoryFilter, directionFilter, bucketFilter],
  );

  const metrics1m = useMemo(() => computeMetrics(trafficEvents, 60_000), [trafficEvents]);
  const metrics5m = useMemo(() => computeMetrics(trafficEvents, 300_000), [trafficEvents]);
  const metrics15m = useMemo(() => computeMetrics(trafficEvents, 900_000), [trafficEvents]);

  if (!isConnected) return null;

  // Not yet fetched since connect: distinguish "still loading" from the
  // "loaded, and it's actually disabled" case below so a slow ESP32 link
  // doesn't read as disabled.
  if (trafficDebugStatus === null) {
    return (
      <section className={styles.card}>
        <div className={styles.header}>
          <h3 className={styles.title}>Traffic Monitor</h3>
        </div>
        <p className={styles.muted}>Loading traffic monitor…</p>
      </section>
    );
  }

  if (!trafficDebugStatus.config.enabled) {
    return (
      <section className={styles.card}>
        <div className={styles.header}>
          <h3 className={styles.title}>Traffic Monitor</h3>
        </div>
        <p className={styles.muted}>Traffic debug is disabled. Enable it in Config to view monitor data.</p>
      </section>
    );
  }

  return (
    <div className={styles.stack}>
      <section className={styles.card}>
        <div className={styles.header}>
          <h3 className={styles.title}>Traffic Summary</h3>
          <span className={styles.muted}>{trafficEvents.length} events buffered</span>
        </div>

        <div className={styles.filterRow}>
          <label className={styles.filterLabel}>
            Category
            <select className={styles.filterSelect} value={categoryFilter} onChange={(e) => setCategoryFilter(e.target.value as TrafficCategory | 'all')}>
              <option value="all">All</option>
              <option value="beacon">Beacon</option>
              <option value="timesync">Timesync</option>
              <option value="routing">Routing</option>
              <option value="ack">ACK</option>
              <option value="chat">Chat</option>
              <option value="maintenance">Maintenance</option>
              <option value="other">Other</option>
            </select>
          </label>

          <label className={styles.filterLabel}>
            Direction
            <select className={styles.filterSelect} value={directionFilter} onChange={(e) => setDirectionFilter(e.target.value as TrafficDirection | 'all')}>
              <option value="all">All</option>
              <option value="tx">TX</option>
              <option value="rx">RX</option>
            </select>
          </label>

          <label className={styles.filterLabel}>
            Bucket
            <select className={styles.filterSelect} value={bucketFilter} onChange={(e) => setBucketFilter(e.target.value as AirtimeBucket | 'all')}>
              <option value="all">All</option>
              <option value="broadcast">Broadcast</option>
              <option value="normal">Normal</option>
              <option value="critical">Critical</option>
            </select>
          </label>
        </div>

        <div className={styles.windows}>
          <MetricsWindow label="1 minute" metrics={metrics1m} />
          <MetricsWindow label="5 minutes" metrics={metrics5m} />
          <MetricsWindow label="15 minutes" metrics={metrics15m} />
        </div>
      </section>

      <section className={styles.card}>
        <div className={styles.header}>
          <h3 className={styles.title}>Recent Events</h3>
          <span className={styles.muted}>{filteredEvents.length} matching current filters</span>
        </div>

        {filteredEvents.length === 0 ? (
          <div className={styles.empty}>No events match the current filters.</div>
        ) : (
          <div className={styles.eventsWrap}>
            <table className={styles.eventTable}>
              <thead>
                <tr>
                  <th style={{ width: '56px' }}>Seq</th>
                  <th style={{ width: '124px' }}>Time</th>
                  <th style={{ width: '42px' }}>Dir</th>
                  <th style={{ width: '90px' }}>Category</th>
                  <th>Type</th>
                  <th style={{ width: '84px' }}>Bucket</th>
                  <th style={{ width: '84px', textAlign: 'right' }}>Airtime</th>
                </tr>
              </thead>
              <tbody>
                {filteredEvents.slice(-50).reverse().map((e) => (
                  <tr key={e.seq}>
                    <td className={styles.mono}>{e.seq}</td>
                    <td>{new Date(e.timestampMs).toLocaleTimeString()}</td>
                    <td>{e.direction.toUpperCase()}</td>
                    <td>{e.category}</td>
                    <td className={styles.truncate}>{e.packetType}</td>
                    <td>{e.airtimeBucket}</td>
                    <td className={`${styles.right} ${styles.mono}`}>{formatAirtime(e.airtimeDebitUs)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </section>
    </div>
  );
}
