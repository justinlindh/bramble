import { useEffect, useState, useMemo } from 'react';
import { useStore } from '../../store/index';
import { loadTrafficDebugStatus, loadTrafficEvents } from '../../store/actions';
import type { TrafficEvent, TrafficCategory, TrafficDirection, AirtimeBucket } from '../../types/bramble';
import styles from './Stats.module.css';

interface RollingMetrics {
  totalAirtimeUs: number;
  categoryBreakdown: Record<TrafficCategory, number>;
  bucketBreakdown: Record<AirtimeBucket, number>;
  eventCount: number;
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

  return {
    totalAirtimeUs,
    categoryBreakdown,
    bucketBreakdown,
    eventCount: filtered.length,
  };
}

function formatAirtime(us: number): string {
  if (us < 1000) return `${us}μs`;
  if (us < 1_000_000) return `${(us / 1000).toFixed(1)}ms`;
  return `${(us / 1_000_000).toFixed(2)}s`;
}

export function TrafficMonitor() {
  const trafficDebugStatus = useStore((s) => s.trafficDebugStatus);
  const trafficEvents = useStore((s) => s.trafficEvents);
  const isConnected = useStore((s) => s.connectionState === 'connected');

  const [categoryFilter, setCategoryFilter] = useState<TrafficCategory | 'all'>('all');
  const [directionFilter, setDirectionFilter] = useState<TrafficDirection | 'all'>('all');
  const [bucketFilter, setBucketFilter] = useState<AirtimeBucket | 'all'>('all');
  const [lastBackfillSeq, setLastBackfillSeq] = useState<number>(0);

  // Initial load and backfill on reconnect
  useEffect(() => {
    if (!isConnected) return;
    void loadTrafficDebugStatus();

    // Backfill missed events
    const backfillSeq = trafficEvents.length > 0 ? Math.max(...trafficEvents.map((e) => e.seq)) : 0;
    if (backfillSeq > lastBackfillSeq) {
      void loadTrafficEvents(lastBackfillSeq);
      setLastBackfillSeq(backfillSeq);
    }
  }, [isConnected]);

  // Filtered events
  const filteredEvents = useMemo(() => {
    return trafficEvents.filter((e) => {
      if (categoryFilter !== 'all' && e.category !== categoryFilter) return false;
      if (directionFilter !== 'all' && e.direction !== directionFilter) return false;
      if (bucketFilter !== 'all' && e.airtimeBucket !== bucketFilter) return false;
      return true;
    });
  }, [trafficEvents, categoryFilter, directionFilter, bucketFilter]);

  // Rolling metrics
  const metrics1m = useMemo(() => computeMetrics(trafficEvents, 60_000), [trafficEvents]);
  const metrics5m = useMemo(() => computeMetrics(trafficEvents, 300_000), [trafficEvents]);
  const metrics15m = useMemo(() => computeMetrics(trafficEvents, 900_000), [trafficEvents]);

  if (!isConnected) {
    return null;
  }

  if (!trafficDebugStatus || !trafficDebugStatus.config.enabled) {
    return (
      <div className={styles.card}>
        <h3>Traffic Monitor</h3>
        <p className={styles.hint}>Traffic debug is disabled. Enable it in Config to see live traffic data.</p>
      </div>
    );
  }

  const renderMetricsTable = (label: string, metrics: RollingMetrics) => {
    const total = metrics.totalAirtimeUs;
    if (total === 0) {
      return (
        <div>
          <strong>{label}:</strong> No events
        </div>
      );
    }

    return (
      <div style={{ marginBottom: '1rem' }}>
        <strong>{label}:</strong> {formatAirtime(total)} ({metrics.eventCount} events)
        <table style={{ width: '100%', marginTop: '0.5rem', fontSize: '0.85em' }}>
          <thead>
            <tr>
              <th style={{ textAlign: 'left' }}>Category</th>
              <th style={{ textAlign: 'right' }}>Airtime</th>
              <th style={{ textAlign: 'right' }}>%</th>
            </tr>
          </thead>
          <tbody>
            {Object.entries(metrics.categoryBreakdown)
              .filter(([_, us]) => us > 0)
              .sort(([_, a], [__, b]) => b - a)
              .map(([cat, us]) => (
                <tr key={cat}>
                  <td>{cat}</td>
                  <td style={{ textAlign: 'right' }}>{formatAirtime(us)}</td>
                  <td style={{ textAlign: 'right' }}>{((100 * us) / total).toFixed(1)}%</td>
                </tr>
              ))}
          </tbody>
        </table>
        <table style={{ width: '100%', marginTop: '0.5rem', fontSize: '0.85em' }}>
          <thead>
            <tr>
              <th style={{ textAlign: 'left' }}>Bucket</th>
              <th style={{ textAlign: 'right' }}>Airtime</th>
              <th style={{ textAlign: 'right' }}>%</th>
            </tr>
          </thead>
          <tbody>
            {Object.entries(metrics.bucketBreakdown)
              .filter(([_, us]) => us > 0)
              .sort(([_, a], [__, b]) => b - a)
              .map(([bucket, us]) => (
                <tr key={bucket}>
                  <td>{bucket}</td>
                  <td style={{ textAlign: 'right' }}>{formatAirtime(us)}</td>
                  <td style={{ textAlign: 'right' }}>{((100 * us) / total).toFixed(1)}%</td>
                </tr>
              ))}
          </tbody>
        </table>
      </div>
    );
  };

  return (
    <div className={styles.card}>
      <h3>Traffic Monitor</h3>

      {/* Filters */}
      <div style={{ display: 'flex', gap: '1rem', marginBottom: '1rem', flexWrap: 'wrap' }}>
        <label>
          Category:
          <select value={categoryFilter} onChange={(e) => setCategoryFilter(e.target.value as any)}>
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

        <label>
          Direction:
          <select value={directionFilter} onChange={(e) => setDirectionFilter(e.target.value as any)}>
            <option value="all">All</option>
            <option value="tx">TX</option>
            <option value="rx">RX</option>
          </select>
        </label>

        <label>
          Bucket:
          <select value={bucketFilter} onChange={(e) => setBucketFilter(e.target.value as any)}>
            <option value="all">All</option>
            <option value="broadcast">Broadcast</option>
            <option value="normal">Normal</option>
            <option value="critical">Critical</option>
          </select>
        </label>
      </div>

      {/* Rolling metrics */}
      <div style={{ marginBottom: '1rem' }}>
        {renderMetricsTable('1 minute', metrics1m)}
        {renderMetricsTable('5 minutes', metrics5m)}
        {renderMetricsTable('15 minutes', metrics15m)}
      </div>

      {/* Recent events list (filtered) */}
      <div>
        <strong>Recent Events ({filteredEvents.length}):</strong>
        {filteredEvents.length === 0 ? (
          <p className={styles.hint}>No events match the current filters.</p>
        ) : (
          <div style={{ maxHeight: '300px', overflowY: 'auto', fontSize: '0.85em', marginTop: '0.5rem' }}>
            <table style={{ width: '100%' }}>
              <thead>
                <tr>
                  <th>Seq</th>
                  <th>Time</th>
                  <th>Dir</th>
                  <th>Category</th>
                  <th>Type</th>
                  <th>Bucket</th>
                  <th>Airtime</th>
                </tr>
              </thead>
              <tbody>
                {filteredEvents.slice(-50).reverse().map((e) => (
                  <tr key={e.seq}>
                    <td>{e.seq}</td>
                    <td>{new Date(e.timestampMs).toLocaleTimeString()}</td>
                    <td>{e.direction.toUpperCase()}</td>
                    <td>{e.category}</td>
                    <td>{e.packetType}</td>
                    <td>{e.airtimeBucket}</td>
                    <td>{formatAirtime(e.airtimeDebitUs)}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        )}
      </div>
    </div>
  );
}
