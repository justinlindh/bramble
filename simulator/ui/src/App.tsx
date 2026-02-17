import React from 'react';
import { PlaybackControls } from './components/PlaybackControls';
import { MeshCanvas } from './components/MeshCanvas';
import { MetricsDashboard } from './components/MetricsDashboard';
import { EventLog } from './components/EventLog';
import { ScenarioLoader } from './components/ScenarioLoader';
import { useSimulation } from './hooks/useSimulation';

export default function App() {
  const { state } = useSimulation();

  return (
    <div style={{
      display: 'flex',
      flexDirection: 'column',
      height: '100vh',
      width: '100vw',
      overflow: 'hidden',
      background: '#0d1117',
    }}>
      {/* Top: header with playback controls + scenario loader */}
      <div style={{ display: 'flex', alignItems: 'center', gap: '16px', paddingRight: '16px', background: '#161b22', borderBottom: '1px solid #30363d', flexShrink: 0 }}>
        <div style={{ flex: 1 }}>
          <PlaybackControls
            running={state.running}
            connected={state.connected}
            currentTime={state.currentTime}
          />
        </div>
        <ScenarioLoader />
      </div>

      {/* Middle: canvas + metrics sidebar */}
      <div style={{
        flex: 1,
        display: 'flex',
        overflow: 'hidden',
        minHeight: 0,
      }}>
        <MeshCanvas nodes={state.nodes} radioRange={150} />
        <MetricsDashboard metrics={state.metrics} />
      </div>

      {/* Bottom: event log */}
      <EventLog events={state.events} />
    </div>
  );
}
