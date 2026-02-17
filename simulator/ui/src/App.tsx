import { PlaybackControls } from './components/PlaybackControls';
import { MeshCanvas } from './components/MeshCanvas';
import { MetricsDashboard } from './components/MetricsDashboard';
import { EventLog } from './components/EventLog';
import { ScenarioLoader } from './components/ScenarioLoader';
import { useSimulation } from './hooks/useSimulation';
import './App.css';

export default function App() {
  const { state, ws } = useSimulation();

  function handleLoadScenario(scenario: string) {
    if (ws && ws.readyState === WebSocket.OPEN) {
      ws.send(JSON.stringify({ type: 'start', scenario }));
    }
  }

  return (
    <div className="app">
      <header className="app-header">
        <div className="app-header-controls">
          <PlaybackControls
            running={state.running}
            connected={state.connected}
            currentTime={state.currentTime}
          />
        </div>
        <ScenarioLoader onLoad={handleLoadScenario} />
      </header>

      <main className="app-main">
        <div className="app-canvas">
          <MeshCanvas nodes={state.nodes} radioRange={150} />
        </div>
        <aside className="app-sidebar">
          <MetricsDashboard metrics={state.metrics} />
          <EventLog events={state.events} />
        </aside>
      </main>
    </div>
  );
}
