import { PlaybackControls } from './components/PlaybackControls';
import { MeshCanvas } from './components/MeshCanvas';
import { MetricsDashboard } from './components/MetricsDashboard';
import { EventLog } from './components/EventLog';
import { ScenarioLoader } from './components/ScenarioLoader';
import { useSimulation } from './hooks/useSimulation';
import './App.css';

export default function App() {
  const { state } = useSimulation();

  return (
    <div className="app">
      {/* Top: header with playback controls + scenario loader */}
      <header className="app-header">
        <div className="app-header-controls">
          <PlaybackControls
            running={state.running}
            connected={state.connected}
            currentTime={state.currentTime}
          />
        </div>
        <ScenarioLoader />
      </header>

      {/* Middle: canvas + metrics sidebar */}
      <main className="app-main">
        <div className="app-canvas">
          <MeshCanvas nodes={state.nodes} radioRange={150} />
        </div>
        <aside className="app-sidebar">
          <MetricsDashboard metrics={state.metrics} />
        </aside>
      </main>

      {/* Bottom: event log */}
      <EventLog events={state.events} />
    </div>
  );
}
