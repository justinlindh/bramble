import type { Metrics } from '../types';

interface MetricsDashboardProps {
  metrics: Metrics | null;
}

interface MetricCardProps {
  label: string;
  value: string;
  subValue?: string;
  color: string;
  icon: string;
}

function MetricCard({ label, value, subValue, color, icon }: MetricCardProps) {
  return (
    <div style={{
      background: '#161b22',
      border: '1px solid #30363d',
      borderLeft: `3px solid ${color}`,
      borderRadius: '6px',
      padding: '10px 12px',
      marginBottom: '8px',
    }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: '6px', marginBottom: '6px' }}>
        <span style={{ fontSize: '13px' }}>{icon}</span>
        <span style={{ fontSize: '10px', color: '#8b949e', textTransform: 'uppercase', letterSpacing: '0.08em', fontWeight: 500 }}>
          {label}
        </span>
      </div>
      <div style={{ fontSize: '22px', fontWeight: 700, color: '#e6edf3', lineHeight: 1, marginBottom: '2px' }}>
        {value}
      </div>
      {subValue && (
        <div style={{ fontSize: '10px', color: '#8b949e', marginTop: '4px' }}>{subValue}</div>
      )}
    </div>
  );
}

function SmallMetric({ label, value, color }: { label: string; value: string; color: string }) {
  return (
    <div style={{
      display: 'flex',
      justifyContent: 'space-between',
      padding: '3px 0',
      fontSize: '11px',
      fontFamily: 'monospace',
    }}>
      <span style={{ color: '#8b949e' }}>{label}</span>
      <span style={{ color, fontWeight: 600 }}>{value}</span>
    </div>
  );
}

function healthScoreColor(score: number): string {
  if (score >= 90) return '#3fb950';
  if (score >= 70) return '#f0883e';
  return '#f85149';
}

export function MetricsDashboard({ metrics }: MetricsDashboardProps) {
  return (
    <div style={{
      background: '#0d1117',
      padding: '12px',
      overflowY: 'auto',
    }}>
      <h2 style={{
        fontSize: '11px',
        fontWeight: 600,
        color: '#8b949e',
        textTransform: 'uppercase',
        letterSpacing: '0.1em',
        marginBottom: '10px',
        paddingBottom: '8px',
        borderBottom: '1px solid #21262d',
        margin: '0 0 10px 0',
      }}>
        Metrics
      </h2>

      {metrics === null ? (
        <div style={{
          color: '#6e7681',
          fontSize: '13px',
          textAlign: 'center',
          marginTop: '30px',
          lineHeight: 1.6,
        }}>
          <div style={{ fontSize: '24px', marginBottom: '8px' }}>📡</div>
          Waiting for data...
        </div>
      ) : (
        <>
          {/* Network Health Score Badge */}
          <div style={{
            background: '#161b22',
            border: '1px solid #30363d',
            borderRadius: '8px',
            padding: '12px',
            marginBottom: '10px',
            textAlign: 'center',
          }}>
            <div style={{ fontSize: '10px', color: '#8b949e', textTransform: 'uppercase', marginBottom: '6px' }}>
              Network Health
            </div>
            <div style={{
              fontSize: '32px',
              fontWeight: 800,
              color: healthScoreColor(metrics.deliveryRate),
              lineHeight: 1,
            }}>
              {metrics.deliveryRate.toFixed(0)}
            </div>
            <div style={{
              display: 'inline-block',
              marginTop: '6px',
              padding: '2px 10px',
              borderRadius: '10px',
              fontSize: '10px',
              fontWeight: 600,
              color: '#fff',
              background: healthScoreColor(metrics.deliveryRate),
            }}>
              {metrics.deliveryRate >= 90 ? 'HEALTHY' : metrics.deliveryRate >= 70 ? 'DEGRADED' : 'CRITICAL'}
            </div>
          </div>

          <MetricCard
            label="Message Delivery"
            value={`${metrics.deliveryRate.toFixed(1)}%`}
            subValue={`${metrics.delivered} of ${metrics.messagesSent} messages`}
            color="#3fb950"
            icon="✅"
          />
          <MetricCard
            label="Avg Latency"
            value={`${metrics.avgLatencyMs.toFixed(2)}`}
            subValue="milliseconds"
            color="#58a6ff"
            icon="⏱"
          />
          <MetricCard
            label="Active Nodes"
            value={String(metrics.activeNodes)}
            color="#d2a8ff"
            icon="📡"
          />
          <MetricCard
            label="Total Packets"
            value={String(metrics.totalPackets)}
            subValue={`${metrics.delivered} delivered`}
            color="#f0883e"
            icon="📦"
          />
          <MetricCard
            label="Dropped"
            value={String(metrics.dropped)}
            subValue={metrics.totalPackets > 0 ? `${((metrics.dropped / metrics.totalPackets) * 100).toFixed(1)}% loss` : undefined}
            color="#f85149"
            icon="🔻"
          />

          {/* Enhanced Metrics */}
          {(metrics.retried != null || metrics.deliveredOnRetry != null || metrics.dedupDropped != null ||
            metrics.airtimeDeferred != null || metrics.fragmentsSent != null || metrics.cryptoEncrypted != null) && (
            <div style={{
              background: '#161b22',
              border: '1px solid #30363d',
              borderRadius: '6px',
              padding: '10px 12px',
              marginBottom: '8px',
            }}>
              <div style={{
                fontSize: '10px', color: '#8b949e', textTransform: 'uppercase',
                letterSpacing: '0.08em', marginBottom: '6px', fontWeight: 600,
              }}>
                Component Integration
              </div>
              {(metrics.deliveredOnRetry != null && metrics.deliveredOnRetry > 0) && (
                <SmallMetric label="Success on retry" value={String(metrics.deliveredOnRetry)} color="#3fb950" />
              )}
              {(metrics.retried != null && metrics.retried > 0) && (
                <SmallMetric label="Retried" value={String(metrics.retried)} color="#f0883e" />
              )}
              {(metrics.dedupDropped != null && metrics.dedupDropped > 0) && (
                <SmallMetric label="Duplicates caught" value={String(metrics.dedupDropped)} color="#58a6ff" />
              )}
              {(metrics.airtimeDeferred != null && metrics.airtimeDeferred > 0) && (
                <SmallMetric label="Airtime throttled" value={String(metrics.airtimeDeferred)} color="#d2a8ff" />
              )}
              {(metrics.fragmentsSent != null && metrics.fragmentsSent > 0) && (
                <SmallMetric label="Fragments sent" value={String(metrics.fragmentsSent)} color="#f0883e" />
              )}
              {(metrics.fragmentsReassembled != null && metrics.fragmentsReassembled > 0) && (
                <SmallMetric label="Fragments reassembled" value={String(metrics.fragmentsReassembled)} color="#3fb950" />
              )}
              {(metrics.cryptoEncrypted != null && metrics.cryptoEncrypted > 0) && (
                <SmallMetric label="Encrypted" value={String(metrics.cryptoEncrypted)} color="#58a6ff" />
              )}
              {(metrics.cryptoDecrypted != null && metrics.cryptoDecrypted > 0) && (
                <SmallMetric label="Decrypted" value={String(metrics.cryptoDecrypted)} color="#58a6ff" />
              )}
            </div>
          )}

          <div style={{
            padding: '8px',
            background: '#161b22',
            border: '1px solid #30363d',
            borderRadius: '6px',
            fontSize: '11px',
            color: '#6e7681',
            fontFamily: 'monospace',
          }}>
            t = {(metrics.timestamp_us / 1_000_000).toFixed(3)}s
          </div>
        </>
      )}
    </div>
  );
}
