import React from 'react';
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
      padding: '14px 16px',
      marginBottom: '10px',
    }}>
      <div style={{ display: 'flex', alignItems: 'center', gap: '6px', marginBottom: '8px' }}>
        <span style={{ fontSize: '14px' }}>{icon}</span>
        <span style={{ fontSize: '11px', color: '#8b949e', textTransform: 'uppercase', letterSpacing: '0.08em', fontWeight: 500 }}>
          {label}
        </span>
      </div>
      <div style={{ fontSize: '28px', fontWeight: 700, color: '#e6edf3', lineHeight: 1, marginBottom: '4px' }}>
        {value}
      </div>
      {subValue && (
        <div style={{ fontSize: '11px', color: '#8b949e', marginTop: '4px' }}>{subValue}</div>
      )}
    </div>
  );
}

export function MetricsDashboard({ metrics }: MetricsDashboardProps) {
  return (
    <aside style={{
      width: '280px',
      flexShrink: 0,
      background: '#0d1117',
      borderLeft: '1px solid #21262d',
      padding: '16px',
      overflowY: 'auto',
    }}>
      <h2 style={{
        fontSize: '12px',
        fontWeight: 600,
        color: '#8b949e',
        textTransform: 'uppercase',
        letterSpacing: '0.1em',
        marginBottom: '14px',
        paddingBottom: '10px',
        borderBottom: '1px solid #21262d',
      }}>
        Metrics
      </h2>

      {metrics === null ? (
        <div style={{
          color: '#6e7681',
          fontSize: '13px',
          textAlign: 'center',
          marginTop: '40px',
          lineHeight: 1.6,
        }}>
          <div style={{ fontSize: '24px', marginBottom: '8px' }}>📡</div>
          Waiting for data...
        </div>
      ) : (
        <>
          <MetricCard
            label="Delivery Rate"
            value={`${metrics.deliveryRate.toFixed(1)}%`}
            subValue={`${metrics.delivered} of ${metrics.totalPackets} packets`}
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

          <div style={{
            marginTop: '16px',
            padding: '10px',
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
    </aside>
  );
}
