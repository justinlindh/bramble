import React from 'react';

interface PlaybackControlsProps {
  running: boolean;
  connected: boolean;
  currentTime: number; // microseconds
}

function formatTime(us: number): string {
  if (us === 0) return '0.000 s';
  const seconds = us / 1_000_000;
  if (seconds < 60) return `${seconds.toFixed(3)} s`;
  const mins = Math.floor(seconds / 60);
  const secs = (seconds % 60).toFixed(3).padStart(6, '0');
  return `${mins}m ${secs}s`;
}

export function PlaybackControls({ running, connected, currentTime }: PlaybackControlsProps) {
  return (
    <header style={{
      background: '#161b22',
      borderBottom: '1px solid #30363d',
      padding: '0 20px',
      height: '52px',
      display: 'flex',
      alignItems: 'center',
      justifyContent: 'space-between',
      flexShrink: 0,
    }}>
      {/* Title */}
      <div style={{ display: 'flex', alignItems: 'center', gap: '10px' }}>
        <svg width="22" height="22" viewBox="0 0 24 24" fill="none" style={{ flexShrink: 0 }}>
          <circle cx="12" cy="12" r="10" stroke="#58a6ff" strokeWidth="1.5" />
          <circle cx="12" cy="12" r="6"  stroke="#58a6ff" strokeWidth="1.5" strokeDasharray="3 2" />
          <circle cx="12" cy="12" r="2"  fill="#58a6ff" />
          <circle cx="5"  cy="12" r="1.5" fill="#3fb950" />
          <circle cx="19" cy="12" r="1.5" fill="#3fb950" />
        </svg>
        <span style={{ fontSize: '15px', fontWeight: 600, color: '#e6edf3', letterSpacing: '0.02em' }}>
          Bramble Mesh Simulator
        </span>
      </div>

      {/* Center: simulation time */}
      <div style={{
        fontFamily: "'Fira Code', 'Consolas', monospace",
        fontSize: '13px',
        color: '#8b949e',
        background: '#0d1117',
        border: '1px solid #30363d',
        borderRadius: '6px',
        padding: '4px 14px',
        letterSpacing: '0.05em',
      }}>
        <span style={{ color: '#8b949e' }}>t = </span>
        <span style={{ color: '#58a6ff', fontWeight: 600 }}>{formatTime(currentTime)}</span>
      </div>

      {/* Right: status indicator */}
      <div style={{ display: 'flex', alignItems: 'center', gap: '8px' }}>
        <div style={{
          width: '8px',
          height: '8px',
          borderRadius: '50%',
          background: !connected ? '#6e7681' : running ? '#3fb950' : '#f85149',
          boxShadow: !connected ? 'none' : running ? '0 0 6px #3fb950' : '0 0 6px #f85149',
          transition: 'background 0.3s, box-shadow 0.3s',
        }} />
        <span style={{ fontSize: '12px', color: '#8b949e' }}>
          {!connected ? 'Disconnected' : running ? 'Running' : 'Stopped'}
        </span>
      </div>
    </header>
  );
}
