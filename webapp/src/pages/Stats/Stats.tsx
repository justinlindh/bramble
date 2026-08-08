import { useEffect, useCallback, useState } from 'react';
import { useStore } from '../../store/index';
import { loadStatus, loadAirtime } from '../../store/actions';
import { AirtimeCard } from './AirtimeCard';
import { CounterGrid } from './CounterGrid';
import { SystemInfo } from './SystemInfo';
import { NetworkReach } from './NetworkReach';
import { RollCallPanel } from './RollCallPanel';
import { TrafficMonitor } from './TrafficMonitor';
import styles from './Stats.module.css';

const REFRESH_INTERVAL_MS = 5_000;

export function Stats() {
  const status      = useStore(s => s.status);
  const airtime     = useStore(s => s.airtime);
  const config      = useStore(s => s.config);
  const isConnected = useStore(s => s.connectionState === 'connected');

  const [refreshing, setRefreshing] = useState(false);
  const [lastRefresh, setLastRefresh] = useState<Date | null>(null);

  const refresh = useCallback(async () => {
    if (!isConnected) return;
    setRefreshing(true);
    try {
      await Promise.all([loadStatus(), loadAirtime()]);
      setLastRefresh(new Date());
    } finally {
      setRefreshing(false);
    }
  }, [isConnected]);

  // Auto-refresh every 5 s while tab is mounted
  useEffect(() => {
    if (!isConnected) return;
    void refresh();
    const id = setInterval(() => void refresh(), REFRESH_INTERVAL_MS);
    return () => clearInterval(id);
  }, [isConnected, refresh]);

  // Immediate refresh when the browser tab regains visibility
  useEffect(() => {
    const handleVisibility = () => {
      if (document.visibilityState === 'visible') void refresh();
    };
    document.addEventListener('visibilitychange', handleVisibility);
    return () => document.removeEventListener('visibilitychange', handleVisibility);
  }, [refresh]);

  if (!isConnected) {
    return (
      <div className={styles.stats}>
        <p className={styles.empty}>Connect to a node to view statistics.</p>
      </div>
    );
  }

  if (!status) {
    return (
      <div className={styles.stats}>
        <p className={styles.empty}>Loading statistics…</p>
      </div>
    );
  }

  return (
    <div className={styles.stats}>
      {/* Toolbar row */}
      <div className={styles.toolbar}>
        {lastRefresh && (
          <span className={styles.lastRefresh}>
            Updated {lastRefresh.toLocaleTimeString()}
          </span>
        )}
        <button
          className={styles.refreshBtn}
          onClick={() => void refresh()}
          disabled={refreshing}
          aria-label="Refresh statistics"
        >
          {refreshing ? '↻' : '↺'} Refresh
        </button>
      </div>

      {/* Packet counters + delta indicators */}
      <CounterGrid status={status} airtime={airtime} />

      {/* Airtime budget bars */}
      {airtime && <AirtimeCard airtime={airtime} />}

      {/* System info: uptime, heap, firmware, address, pubkey */}
      {config && <SystemInfo status={status} config={config} />}

      {/* Network Reach probe */}
      <NetworkReach />

      {/* Attested roll call */}
      <RollCallPanel />

      {/* Traffic Monitor */}
      <TrafficMonitor />
    </div>
  );
}
