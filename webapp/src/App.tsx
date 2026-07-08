import { type ReactNode, Suspense, lazy, useEffect, useRef } from 'react';
import { useStore } from './store/index';
import { disconnect, loadConnectionCapabilities, loadNeighbors, loadNetworkKeyStatus } from './store/actions';
import { usePoll } from './hooks/usePoll';
import { isAndroidShell } from './utils/platform';
import { ConnectionOverlay } from './components/ConnectionOverlay';
import { UnprovisionedBanner } from './components/UnprovisionedBanner';
import { StatusDot } from './components/StatusDot';
import { ErrorBoundary } from './components/ErrorBoundary';
import { ToastContainer, showToast, dismissToast } from './components/Toast';
import { IconChat, IconNodes, IconConfig, IconStats, IconMap } from './components/Icons';
import { Chat } from './pages/Chat/Chat';
import { Nodes } from './pages/Nodes/Nodes';
import { Config } from './pages/Config/Config';
import { Stats } from './pages/Stats/Stats';
import styles from './styles/App.module.css';

// Lazy-load the Map page: leaflet (~150 kB) is only needed when the user opens
// the Map tab. The Suspense fallback renders a plain loading indicator while the
// chunk fetches; the ErrorBoundary above catches any import failure gracefully.
const Map = lazy(() => import('./pages/Map/Map').then(m => ({ default: m.Map })));

type Tab = 'chat' | 'nodes' | 'map' | 'config' | 'stats';

export function tabFromShortcut(key: string): Tab | null {
  if (key === '1') return 'chat';
  if (key === '2') return 'nodes';
  if (key === '3') return 'map';
  if (key === '4') return 'config';
  if (key === '5') return 'stats';
  return null;
}

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
      {activeTab === 'map'    && (
        <Suspense fallback={<div style={{ padding: '2rem', opacity: 0.6 }}>Loading map…</div>}>
          <Map />
        </Suspense>
      )}
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
  const config = useStore(s => s.config);
  const isConnected = connectionState === 'connected';
  const prevState = useRef(connectionState);
  const errorToastId = useRef<string | null>(null);

  // initMessageStore is now called during connect() with the node address

  // Global neighbor poll for presence status (works from any tab)
  usePoll(isConnected ? loadNeighbors : () => Promise.resolve(), 10_000);

  // Global provisioning poll: keeps the UNPROVISIONED (inert) banner live on
  // every tab and makes it vanish the moment a key is set on this node.
  usePoll(isConnected ? loadNetworkKeyStatus : () => Promise.resolve(), 10_000);

  useEffect(() => {
    loadConnectionCapabilities();
  }, []);

  // Android shell: notification taps deep-link into the tapped conversation.
  // The native side calls window.brambleOpenConversation(conversationId)
  // once the WebView is ready.
  useEffect(() => {
    if (!isAndroidShell()) return;
    window.brambleOpenConversation = (conversationId: string) => {
      const s = useStore.getState();
      s.setActiveTab('chat');
      s.setActiveConversation(conversationId);
    };
    return () => { delete window.brambleOpenConversation; };
  }, []);

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

  // Global keyboard shortcuts
  useEffect(() => {
    const onKeyDown = (e: KeyboardEvent) => {
      if (e.ctrlKey) {
        const tab = tabFromShortcut(e.key);
        if (tab) {
          e.preventDefault();
          setActiveTab(tab);
          return;
        }
      }

      if (e.key === '/') {
        const t = e.target as HTMLElement | null;
        const tag = t?.tagName?.toLowerCase();
        const editable = t?.getAttribute('contenteditable') === 'true';
        if (tag === 'input' || tag === 'textarea' || editable) return;
        const compose = document.querySelector('[aria-label="Message input"]') as HTMLTextAreaElement | null;
        if (compose) {
          e.preventDefault();
          compose.focus();
        }
      }

      if (e.key === 'Escape') {
        const active = document.activeElement as HTMLElement | null;
        active?.blur();
      }
    };

    window.addEventListener('keydown', onKeyDown);
    return () => window.removeEventListener('keydown', onKeyDown);
  }, [setActiveTab]);

  // Show connection overlay for initial connect, not during auto-reconnect
  const showOverlay = connectionState !== 'connected' && connectionState !== 'error';

  const handleConnectionToggle = () => {
    if (isConnected || connectionState === 'error') {
      disconnect();
    }
  };

  // Get node identifier: name if set, otherwise hex address
  const getNodeIdentifier = (): string | null => {
    if (!config?.identity) return null;
    const name = config.identity.name?.trim();
    if (name && name !== '' && name !== '(unnamed)') {
      return name;
    }
    // Fallback to hex address
    if (config.identity.address) {
      return `0x${config.identity.address.toString(16).toUpperCase().padStart(8, '0')}`;
    }
    return null;
  };

  const nodeIdentifier = getNodeIdentifier();

  return (
    <div className={styles.app}>
      {/* Topbar */}
      <header className={styles.topbar}>
        <span className={styles.brand}>
          <img src="./bramble-logo.png" alt="" className={styles.brandLogo} />
          Bramble
        </span>

        <span className={styles.statusArea}>
          <StatusDot state={connectionState} />
          <span className={styles.statusLabel}>
            {connectionState === 'connected' ? 'Connected'
             : connectionState === 'error' ? 'Reconnecting…'
             : connectionState}
          </span>
          {isConnected && nodeIdentifier && (
            <>
              <span className={styles.statusDivider}>•</span>
              <span className={styles.nodeLabel} title={nodeIdentifier}>
                {nodeIdentifier}
              </span>
            </>
          )}
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

      {/* Prominent, always-visible warning when this node has no network key */}
      <UnprovisionedBanner />

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
