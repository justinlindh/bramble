import { useState } from 'react';

interface PlaybackControlsProps {
  running: boolean;
  connected: boolean;
  ready: boolean;
  currentTime: number; // microseconds
  ws: WebSocket | null;
}

function formatTime(us: number): string {
  if (us === 0) return '0.000 s';
  const seconds = us / 1_000_000;
  if (seconds < 60) return `${seconds.toFixed(3)} s`;
  const mins = Math.floor(seconds / 60);
  const secs = (seconds % 60).toFixed(3).padStart(6, '0');
  return `${mins}m ${secs}s`;
}

const SPEEDS = [0.5, 1, 2, 5, 10, 50, 100];

function sendCmd(ws: WebSocket | null, msg: object) {
  if (ws && ws.readyState === WebSocket.OPEN) {
    ws.send(JSON.stringify(msg));
  }
}

export function PlaybackControls({ running, connected, ready, currentTime, ws }: PlaybackControlsProps) {
  const [paused, setPaused] = useState(true);
  const [speed, setSpeed] = useState(1);

  function handlePlayPause() {
    if (paused) {
      sendCmd(ws, { type: 'play' });
      setPaused(false);
    } else {
      sendCmd(ws, { type: 'pause' });
      setPaused(true);
    }
  }

  function handleRestart() {
    sendCmd(ws, { type: 'restart' });
    setPaused(false);
  }

  function handleSpeed(s: number) {
    setSpeed(s);
    sendCmd(ws, { type: 'speed', value: s });
  }

  const isActive = connected;

  const btnBase: React.CSSProperties = {
    background: '#21262d',
    border: '1px solid #30363d',
    borderRadius: '5px',
    color: '#e6edf3',
    padding: '3px 8px',
    fontSize: '12px',
    cursor: isActive ? 'pointer' : 'not-allowed',
    opacity: isActive ? 1 : 0.4,
    fontFamily: 'monospace',
    transition: 'background 0.15s',
    minWidth: '28px',
  };

  return (
    <header style={{
      background: '#161b22',
      borderBottom: '1px solid #30363d',
      padding: '6px 12px',
      minHeight: '44px',
      display: 'flex',
      alignItems: 'center',
      gap: '12px',
      flexShrink: 0,
      flexWrap: 'wrap',
      overflow: 'hidden',
    }}>
      {/* Title */}
      <div style={{ display: 'flex', alignItems: 'center', gap: '8px', flexShrink: 0 }}>
        <svg width="20" height="20" viewBox="0 0 24 24" fill="none" style={{ flexShrink: 0 }}>
          <circle cx="12" cy="12" r="10" stroke="#58a6ff" strokeWidth="1.5" />
          <circle cx="12" cy="12" r="6"  stroke="#58a6ff" strokeWidth="1.5" strokeDasharray="3 2" />
          <circle cx="12" cy="12" r="2"  fill="#58a6ff" />
          <circle cx="5"  cy="12" r="1.5" fill="#3fb950" />
          <circle cx="19" cy="12" r="1.5" fill="#3fb950" />
        </svg>
        <span style={{ fontSize: '14px', fontWeight: 600, color: '#e6edf3', letterSpacing: '0.02em', whiteSpace: 'nowrap' }}>
          Bramble Mesh Simulator
        </span>
      </div>

      {/* Playback controls + sim time */}
      <div style={{ display: 'flex', alignItems: 'center', gap: '8px', flexShrink: 0 }}>
        {/* Play/Pause */}
        <button
          onClick={handlePlayPause}
          disabled={!isActive}
          title={paused ? 'Play' : 'Pause'}
          style={{ ...btnBase, fontSize: '14px', padding: '3px 10px' }}
        >
          {paused ? '▶' : '⏸'}
        </button>

        {/* Restart */}
        <button
          onClick={handleRestart}
          disabled={!isActive}
          title="Restart"
          style={{ ...btnBase, fontSize: '14px', padding: '3px 10px' }}
        >
          ⟲
        </button>

        {/* Add Node */}
        <button
          onClick={() => sendCmd(ws, { type: 'add_node' })}
          disabled={!isActive}
          title="Add a node near a random existing node"
          style={{ ...btnBase, fontSize: '11px', padding: '3px 8px', background: '#1a7f37', borderColor: '#2ea043' }}
        >
          + Node
        </button>

        {/* Speed buttons */}
        <div style={{ display: 'flex', gap: '3px', alignItems: 'center' }}>
          {SPEEDS.map(s => (
            <button
              key={s}
              onClick={() => handleSpeed(s)}
              disabled={!isActive}
              title={`${s}x speed`}
              style={{
                ...btnBase,
                background: speed === s ? '#1f6feb' : '#21262d',
                borderColor: speed === s ? '#388bfd' : '#30363d',
                padding: '2px 6px',
                fontSize: '11px',
                minWidth: 'unset',
              }}
            >
              {s}×
            </button>
          ))}
        </div>

        {/* Sim time */}
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
      </div>

      {/* Status indicator */}
      <div style={{ display: 'flex', alignItems: 'center', gap: '6px', marginLeft: 'auto', flexShrink: 0, whiteSpace: 'nowrap' }}>
        <div style={{
          width: '8px',
          height: '8px',
          borderRadius: '50%',
          background: !connected ? '#6e7681' : running ? '#3fb950' : ready ? '#f0883e' : '#58a6ff',
          boxShadow: !connected ? 'none' : running ? '0 0 6px #3fb950' : ready ? '0 0 6px #f0883e' : '0 0 6px #58a6ff',
          transition: 'background 0.3s, box-shadow 0.3s',
        }} />
        <span style={{ fontSize: '12px', color: '#8b949e' }}>
          {!connected ? 'Disconnected' : running ? 'Running' : ready ? 'Ready' : 'Completed'}
        </span>
      </div>
    </header>
  );
}
