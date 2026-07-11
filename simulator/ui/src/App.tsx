import { useState, useMemo, useCallback } from 'react';
import { PlaybackControls } from './components/PlaybackControls';
import { MeshCanvas } from './components/MeshCanvas';
import { MetricsDashboard } from './components/MetricsDashboard';
import { EventLog } from './components/EventLog';
import { PathHistory } from './components/PathHistory';
import { NodeHealthCard } from './components/NodeHealthCard';
import { ScenarioLoader } from './components/ScenarioLoader';
import { useSimulation } from './hooks/useSimulation';
import DeviceView from './device/DeviceView';
import type { NeighborRSSI } from './types';
import './App.css';

export default function App() {
  const { state, ws, selectNode, sendButton } = useSimulation();
  const [bottomTab, setBottomTab] = useState<'events' | 'paths'>('events');
  const [mainView, setMainView] = useState<'mesh' | 'devices'>('mesh');
  const deviceCount = state.devices.size;

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

  // Compute per-neighbor RSSI list for the selected node
  const neighborRssi = useMemo((): NeighborRSSI[] => {
    if (!selectedNode) return [];
    const result: NeighborRSSI[] = [];
    for (const [key, lq] of state.linkQuality) {
      const parts = key.split('-');
      if (parts.length !== 2) continue;
      const [a, b] = parts;
      let neighborId: string | null = null;
      if (a === selectedNode.id) neighborId = b;
      else if (b === selectedNode.id) neighborId = a;
      if (!neighborId) continue;
      const neighbor = state.nodes.get(neighborId);
      if (!neighbor || !neighbor.active) continue;
      result.push({ nodeId: neighborId, rssi: lq.rssi, snr: lq.snr });
    }
    // Sort by RSSI descending (strongest first)
    result.sort((a, b) => b.rssi - a.rssi);
    return result;
  }, [selectedNode, state.linkQuality, state.nodes]);

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
        <div className="app-view-tabs">
          <button
            className={`app-view-tab ${mainView === 'mesh' ? 'app-view-tab-active' : ''}`}
            onClick={() => setMainView('mesh')}
          >
            Mesh
          </button>
          <button
            className={`app-view-tab ${mainView === 'devices' ? 'app-view-tab-active' : ''}`}
            onClick={() => setMainView('devices')}
          >
            Devices{deviceCount > 0 ? ` (${deviceCount})` : ''}
          </button>
        </div>
        <ScenarioLoader onLoad={handleLoadScenario} />
      </header>

      {/* Main content area */}
      <div className="app-body">
        {/* Center: mesh canvas or device view */}
        <div className="app-canvas">
          {mainView === 'mesh' ? (
            <MeshCanvas
              nodes={state.nodes}
              radioRange={150}
              events={state.events}
              ws={ws.current}
              deliveryPaths={state.deliveryPaths}
              linkActivity={state.linkActivity}
              brokenLinks={state.brokenLinks}
              linkQuality={state.linkQuality}
              selectedNodeId={state.selectedNodeId}
              onNodeClick={handleNodeClick}
            />
          ) : (
            <DeviceView devices={state.devices} onButton={sendButton} />
          )}
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
                neighborRssi={neighborRssi}
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
