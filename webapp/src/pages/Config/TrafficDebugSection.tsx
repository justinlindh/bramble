import { useState } from 'react';
import { useStore } from '../../store/index';
import { loadTrafficDebugStatus, setTrafficDebugConfig } from '../../store/actions';
import styles from './Config.module.css';

export function TrafficDebugSection() {
  const trafficDebugStatus = useStore((s) => s.trafficDebugStatus);
  const [loading, setLoading] = useState(false);

  const handleToggle = async (field: 'enabled' | 'includeTx' | 'includeRx', value: boolean) => {
    setLoading(true);
    try {
      const update: Record<string, boolean> = {};
      update[field] = value;
      await setTrafficDebugConfig(update);
    } catch (e) {
      alert(`Failed to update: ${(e as Error).message}`);
    } finally {
      setLoading(false);
    }
  };

  const handleSampleRateChange = async (rate: number) => {
    setLoading(true);
    try {
      await setTrafficDebugConfig({ sampleRate: rate });
    } catch (e) {
      alert(`Failed to update sample rate: ${(e as Error).message}`);
    } finally {
      setLoading(false);
    }
  };

  const handleRefresh = async () => {
    setLoading(true);
    try {
      await loadTrafficDebugStatus();
    } finally {
      setLoading(false);
    }
  };

  if (!trafficDebugStatus) {
    return (
      <div>
        <button onClick={handleRefresh} disabled={loading}>
          Load Traffic Debug Status
        </button>
      </div>
    );
  }

  const { config, ringSize, ringUsed, droppedCount, lastSeq } = trafficDebugStatus;
  const usagePct = ringSize > 0 ? Math.round((100 * ringUsed) / ringSize) : 0;

  return (
    <div>
      <div className={styles.formRow}>
        <label>
          <input
            type="checkbox"
            checked={config.enabled}
            onChange={(e) => handleToggle('enabled', e.target.checked)}
            disabled={loading}
          />
          <span>Enable Traffic Debug</span>
        </label>
      </div>

      {config.enabled && (
        <>
          <div className={styles.formRow}>
            <label>
              <input
                type="checkbox"
                checked={config.includeTx}
                onChange={(e) => handleToggle('includeTx', e.target.checked)}
                disabled={loading}
              />
              <span>Include TX</span>
            </label>
          </div>

          <div className={styles.formRow}>
            <label>
              <input
                type="checkbox"
                checked={config.includeRx}
                onChange={(e) => handleToggle('includeRx', e.target.checked)}
                disabled={loading}
              />
              <span>Include RX</span>
            </label>
          </div>

          <div className={styles.formRow}>
            <label>
              Sample Rate: {config.sampleRate}%
              <input
                type="range"
                min="1"
                max="100"
                value={config.sampleRate}
                onChange={(e) => handleSampleRateChange(Number(e.target.value))}
                disabled={loading}
                style={{ width: '100%' }}
              />
            </label>
          </div>

          <div className={styles.hint}>
            Ring buffer: {ringUsed}/{ringSize} ({usagePct}%) · Dropped: {droppedCount} · Last seq: {lastSeq}
          </div>
        </>
      )}
    </div>
  );
}
