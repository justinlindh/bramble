import { useState } from 'react';
import type { ReactNode } from 'react';
import { useStore } from './store/index';
import { disconnect } from './store/actions';
import { ConnectionOverlay } from './components/ConnectionOverlay';
import { StatusDot } from './components/StatusDot';
import { ErrorBoundary } from './components/ErrorBoundary';
import { IconChat, IconNodes, IconConfig, IconStats } from './components/Icons';
import { Chat } from './pages/Chat/Chat';
import { Nodes } from './pages/Nodes/Nodes';
import { Config } from './pages/Config/Config';
import { Stats } from './pages/Stats/Stats';
import styles from './styles/App.module.css';

type Tab = 'chat' | 'nodes' | 'config' | 'stats';

const TABS: { id: Tab; icon: ReactNode; label: string }[] = [
  { id: 'chat',   icon: <IconChat size={18} />,   label: 'Chat'   },
  { id: 'nodes',  icon: <IconNodes size={18} />,  label: 'Nodes'  },
  { id: 'config', icon: <IconConfig size={18} />, label: 'Config' },
  { id: 'stats',  icon: <IconStats size={18} />,  label: 'Stats'  },
];

function TabContent({ activeTab }: { activeTab: Tab }) {
  return (
    <ErrorBoundary>
      {activeTab === 'chat'   && <Chat />}
      {activeTab === 'nodes'  && <Nodes />}
      {activeTab === 'config' && <Config />}
      {activeTab === 'stats'  && <Stats />}
    </ErrorBoundary>
  );
}

export default function App() {
  const [activeTab, setActiveTab] = useState<Tab>('chat');
  const connectionState = useStore(s => s.connectionState);
  const isConnected = connectionState === 'connected';

  // Show the connection overlay whenever we're not in a live connected state
  const showOverlay = connectionState !== 'connected';

  const handleConnectionToggle = () => {
    if (isConnected) {
      disconnect();
    }
    // Connecting / error recovery is triggered from the overlay
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
            {connectionState === 'connected' ? 'Connected' : connectionState}
          </span>
        </span>

        {isConnected && (
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

      {/* Connection overlay (shown when disconnected/connecting) */}
      {showOverlay && <ConnectionOverlay />}
    </div>
  );
}
