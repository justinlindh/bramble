import { type ReactNode, useEffect, useRef } from 'react';
import { useStore } from './store/index';
import { disconnect } from './store/actions';
import { ConnectionOverlay } from './components/ConnectionOverlay';
import { StatusDot } from './components/StatusDot';
import { ErrorBoundary } from './components/ErrorBoundary';
import { ToastContainer, showToast, dismissToast } from './components/Toast';
import { IconChat, IconNodes, IconConfig, IconStats, IconMap } from './components/Icons';
import { Chat } from './pages/Chat/Chat';
import { Nodes } from './pages/Nodes/Nodes';
import { Config } from './pages/Config/Config';
import { Stats } from './pages/Stats/Stats';
import { Map } from './pages/Map/Map';
import styles from './styles/App.module.css';

type Tab = 'chat' | 'nodes' | 'map' | 'config' | 'stats';

const TABS: { id: Tab; icon: ReactNode; label: string }[] = [
  { id: 'chat',   icon: <IconChat size={18} />,   label: 'Chat'   },
  { id: 'nodes',  icon: <IconNodes size={18} />,  label: 'Nodes'  },
  { id: 'map',    icon: <IconMap size={18} />,    label: 'Map'    },
  { id: 'config', icon: <IconConfig size={18} />, label: 'Config' },
  { id: 'stats',  icon: <IconStats size={18} />,  label: 'Stats'  },
];

function TabContent({ activeTab }: { activeTab: Tab }) {
  return (
    <ErrorBoundary>
      {activeTab === 'chat'   && <Chat />}
      {activeTab === 'nodes'  && <Nodes />}
      {activeTab === 'map'    && <Map />}
      {activeTab === 'config' && <Config />}
      {activeTab === 'stats'  && <Stats />}
    </ErrorBoundary>
  );
}

export default function App() {
  const activeTab = useStore(s => s.activeTab) as Tab;
  const setActiveTab = useStore(s => s.setActiveTab);
  const connectionState = useStore(s => s.connectionState);
  const connectionError = useStore(s => s.connectionError);
  const isConnected = connectionState === 'connected';
  const prevState = useRef(connectionState);
  const errorToastId = useRef<string | null>(null);

  // initMessageStore is now called during connect() with the node address

  // Toast notifications for connection state changes
  useEffect(() => {
    const prev = prevState.current;
    prevState.current = connectionState;

    // Clear old error toast when state changes
    if (errorToastId.current && connectionState !== 'error') {
      dismissToast(errorToastId.current);
      errorToastId.current = null;
    }

    if (connectionState === 'error' && connectionError) {
      errorToastId.current = showToast(connectionError, 'warning', 0); // persistent until resolved
    } else if (connectionState === 'connected' && prev === 'error') {
      showToast('Reconnected', 'success', 3000);
    }
  }, [connectionState, connectionError]);

  // Show connection overlay for initial connect, not during auto-reconnect
  const showOverlay = connectionState !== 'connected' && connectionState !== 'error';

  const handleConnectionToggle = () => {
    if (isConnected || connectionState === 'error') {
      disconnect();
    }
  };

  return (
    <div className={styles.app}>
      {/* Topbar */}
      <header className={styles.topbar}>
        <span className={styles.brand}>
          <img src="/bramble-logo.png" alt="" className={styles.brandLogo} />
          Bramble
        </span>

        <span className={styles.statusArea}>
          <StatusDot state={connectionState} />
          <span className={styles.statusLabel}>
            {connectionState === 'connected' ? 'Connected'
             : connectionState === 'error' ? 'Reconnecting…'
             : connectionState}
          </span>
        </span>

        {(isConnected || connectionState === 'error') && (
          <button
            className={styles.disconnectBtn}
            onClick={handleConnectionToggle}
            aria-label="Disconnect"
          >
            Disconnect
          </button>
        )}
      </header>

      {/* Body: sidebar (desktop) + content */}
      <div className={styles.body}>
        {/* Desktop sidebar nav */}
        <nav className={styles.sidebar} aria-label="Main navigation">
          {TABS.map(tab => (
            <button
              key={tab.id}
              className={`${styles.tab} ${activeTab === tab.id ? styles.active : ''}`}
              onClick={() => setActiveTab(tab.id)}
              aria-current={activeTab === tab.id ? 'page' : undefined}
            >
              <span className={styles.tabIcon} aria-hidden="true">{tab.icon}</span>
              <span>{tab.label}</span>
            </button>
          ))}
        </nav>

        {/* Page content */}
        <main className={styles.content}>
          <TabContent activeTab={activeTab} />
        </main>
      </div>

      {/* Mobile bottom tabbar */}
      <nav className={styles.tabbar} aria-label="Main navigation">
        {TABS.map(tab => (
          <button
            key={tab.id}
            className={`${styles.tab} ${activeTab === tab.id ? styles.active : ''}`}
            onClick={() => setActiveTab(tab.id)}
            aria-current={activeTab === tab.id ? 'page' : undefined}
          >
            <span className={styles.tabIcon} aria-hidden="true">{tab.icon}</span>
            <span>{tab.label}</span>
          </button>
        ))}
      </nav>

      {/* Toast notifications */}
      <ToastContainer />

      {/* Connection overlay (shown when disconnected/connecting) */}
      {showOverlay && <ConnectionOverlay />}
    </div>
  );
}
