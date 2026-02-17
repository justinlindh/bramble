import { useState, useMemo, useCallback } from 'react';
import { PlaybackControls } from './components/PlaybackControls';
import { MeshCanvas } from './components/MeshCanvas';
import { MetricsDashboard } from './components/MetricsDashboard';
import { EventLog } from './components/EventLog';
import { PathHistory } from './components/PathHistory';
import { NodeHealthCard } from './components/NodeHealthCard';
import { ScenarioLoader } from './components/ScenarioLoader';
import { useSimulation } from './hooks/useSimulation';
import './App.css';

export default function App() {
  const { state, ws, selectNode } = useSimulation();
  const [bottomTab, setBottomTab] = useState<'events' | 'paths'>('events');

  function handleLoadScenario(scenario: string) {
    const sock = ws.current;
    if (sock && sock.readyState === WebSocket.OPEN) {
      sock.send(JSON.stringify({ type: 'start', scenario }));
    }
  }

  const handleNodeClick = useCallback((nodeId: string) => {
    if (!nodeId) {
      selectNode(null);
    } else {
      selectNode(state.selectedNodeId === nodeId ? null : nodeId);
    }
  }, [selectNode, state.selectedNodeId]);

  const selectedNode = state.selectedNodeId ? state.nodes.get(state.selectedNodeId) ?? null : null;
  const selectedNodeStats = state.selectedNodeId ? state.nodeStats.get(state.selectedNodeId) ?? null : null;

  // Count neighbors for selected node
  const neighborCount = useMemo(() => {
    if (!selectedNode || !selectedNode.active) return 0;
    let count = 0;
    for (const node of state.nodes.values()) {
      if (node.id === selectedNode.id || !node.active) continue;
      const dx = node.x - selectedNode.x;
      const dy = node.y - selectedNode.y;
      if (Math.sqrt(dx * dx + dy * dy) <= 150) count++;
    }
    return count;
  }, [selectedNode, state.nodes]);

  return (
    <div className="app">
      {/* Top bar */}
      <header className="app-header">
        <div className="app-header-controls">
          <PlaybackControls
            running={state.running}
            connected={state.connected}
            ready={state.ready}
            currentTime={state.currentTime}
            ws={ws.current}
          />
        </div>
        <ScenarioLoader onLoad={handleLoadScenario} />
      </header>

      {/* Main content area */}
      <div className="app-body">
        {/* Center: mesh canvas */}
        <div className="app-canvas">
          <MeshCanvas
            nodes={state.nodes}
            radioRange={150}
            events={state.events}
            ws={ws.current}
            deliveryPaths={state.deliveryPaths}
            linkActivity={state.linkActivity}
            brokenLinks={state.brokenLinks}
            selectedNodeId={state.selectedNodeId}
            onNodeClick={handleNodeClick}
          />
        </div>

        {/* Right sidebar */}
        <aside className="app-sidebar">
          <MetricsDashboard metrics={state.metrics} />
          {selectedNode && (
            <div style={{ padding: '0 12px 12px' }}>
              <NodeHealthCard
                node={selectedNode}
                stats={selectedNodeStats}
                neighborCount={neighborCount}
                onClose={() => selectNode(null)}
              />
            </div>
          )}
        </aside>
      </div>

      {/* Bottom panel: tabbed */}
      <div className="app-bottom">
        <div className="app-bottom-tabs">
          <button
            className={`app-tab ${bottomTab === 'events' ? 'app-tab-active' : ''}`}
            onClick={() => setBottomTab('events')}
          >
            Events ({state.events.length})
          </button>
          <button
            className={`app-tab ${bottomTab === 'paths' ? 'app-tab-active' : ''}`}
            onClick={() => setBottomTab('paths')}
          >
            Path History ({state.deliveryRecords.length})
          </button>
        </div>
        <div className="app-bottom-content">
          {bottomTab === 'events' ? (
            <EventLog events={state.events} />
          ) : (
            <PathHistory records={state.deliveryRecords} />
          )}
        </div>
      </div>
    </div>
  );
}
