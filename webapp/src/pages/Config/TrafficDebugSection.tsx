import { useState, useRef, useCallback } from 'react';
import { useStore } from '../../store/index';
import { loadTrafficDebugStatus, setTrafficDebugConfig } from '../../store/actions';
import styles from './TrafficDebugSection.module.css';

export function TrafficDebugSection() {
  const trafficDebugStatus = useStore((s) => s.trafficDebugStatus);
  const [loading, setLoading] = useState(false);
  const [localRate, setLocalRate] = useState<number | null>(null);
  const debounceRef = useRef<ReturnType<typeof setTimeout>>(undefined);

  const handleToggle = async (field: 'enabled' | 'includeTx' | 'includeRx', value: boolean) => {
    setLoading(true);
    try {
      const update: Record<string, boolean> = {};
      update[field] = value;
      await setTrafficDebugConfig(update);
    } catch (e) {
      console.error(`Failed to update: ${(e as Error).message}`);
    } finally {
      setLoading(false);
    }
  };

  const handleSampleRateChange = useCallback((rate: number) => {
    setLocalRate(rate);
    if (debounceRef.current) clearTimeout(debounceRef.current);
    debounceRef.current = setTimeout(async () => {
      setLoading(true);
      try {
        await setTrafficDebugConfig({ sampleRate: rate });
      } catch (e) {
        console.error(`Failed to update sample rate: ${(e as Error).message}`);
      } finally {
        setLoading(false);
        setLocalRate(null);
      }
    }, 300);
  }, []);

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
        <button className={styles.loadBtn} onClick={handleRefresh} disabled={loading}>
          Load Traffic Debug Status
        </button>
      </div>
    );
  }

  const { config, ringSize, ringUsed, droppedCount, lastSeq } = trafficDebugStatus;
  const usagePct = ringSize > 0 ? Math.round((100 * ringUsed) / ringSize) : 0;

  return (
    <div className={styles.section}>
      <div className={styles.row}>
        <span className={styles.label}>Status</span>
        <label className={styles.toggle}>
          <input
            type="checkbox"
            checked={config.enabled}
            onChange={(e) => handleToggle('enabled', e.target.checked)}
            disabled={loading}
          />
          <span>{config.enabled ? 'Enabled' : 'Disabled'}</span>
        </label>
      </div>

      {config.enabled && (
        <>
          <div className={styles.row}>
            <span className={styles.label}>Capture TX</span>
            <label className={styles.toggle}>
              <input
                type="checkbox"
                checked={config.includeTx}
                onChange={(e) => handleToggle('includeTx', e.target.checked)}
                disabled={loading}
              />
              <span>{config.includeTx ? 'On' : 'Off'}</span>
            </label>
          </div>

          <div className={styles.row}>
            <span className={styles.label}>Capture RX</span>
            <label className={styles.toggle}>
              <input
                type="checkbox"
                checked={config.includeRx}
                onChange={(e) => handleToggle('includeRx', e.target.checked)}
                disabled={loading}
              />
              <span>{config.includeRx ? 'On' : 'Off'}</span>
            </label>
          </div>

          <div className={styles.row}>
            <span className={styles.label}>Sample Rate</span>
            <div className={styles.sliderWrap}>
              <input
                className={styles.slider}
                type="range"
                min="1"
                max="100"
                value={localRate ?? config.sampleRate}
                onChange={(e) => handleSampleRateChange(Number(e.target.value))}
                disabled={loading}
              />
              <span className={styles.sliderValue}>{localRate ?? config.sampleRate}%</span>
            </div>
          </div>

          <div className={styles.hint}>
            Ring buffer: {ringUsed}/{ringSize} ({usagePct}%) · Dropped: {droppedCount} · Last seq: {lastSeq}
          </div>
        </>
      )}
    </div>
  );
}
