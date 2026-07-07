// Types shared between the Electron main process, preload, and renderer for
// LAN node discovery. The renderer feature-detects window.brambleDesktop;
// it is undefined in web deployments.

export type DiscoveredNode = {
  /** Full 8-hex uppercase node address from mDNS TXT `addr` (newer firmware only). */
  addrHex?: string;
  /** Node name from mDNS TXT `name` (newer firmware only). */
  name?: string;
  /** mDNS hostname without .local, e.g. "bramble-6eee". */
  hostname: string;
  /** IPv4 address to connect to (the WS endpoint is always ws://<ip>/ws). */
  ip: string;
};

export type BrambleDesktopApi = {
  startDiscovery(): void;
  stopDiscovery(): void;
  /** Subscribes to discovery snapshots. Returns an unsubscribe function. */
  onDiscovered(cb: (nodes: DiscoveredNode[]) => void): () => void;
};

/** IPC channel names shared by the Electron main process and preload. */
export const DISCOVERY_CHANNELS = {
  start: 'discovery:start',
  stop: 'discovery:stop',
  update: 'discovery:update',
} as const;

declare global {
  interface Window {
    brambleDesktop?: BrambleDesktopApi;
  }
}
