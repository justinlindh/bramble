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

/**
 * Native bridge injected by the Android WebView shell (bramble-android,
 * BrambleAndroidBridge). The webapp hands it the active node connection so
 * the shell's background notification service can open its own
 * authenticated WebSocket.
 */
export type BrambleAndroidNativeApi = {
  updateConnection(wsUrl: string, token: string): void;
};

/** One incoming message the shell should raise a native notification for. */
export type NativeMessageNotification = {
  /** Stable per-conversation key, e.g. "dm:DEADBEEF", "ch:0", "broadcast". */
  conversationId: string;
  /** Conversation display name (peer or channel) shown as the notification title. */
  conversationTitle: string;
  /** Sender display name for the MessagingStyle line. */
  sender: string;
  /** Message body. */
  text: string;
  /** Message time in epoch milliseconds. */
  timestamp: number;
};

/**
 * Native message-notification bridge injected by the Android shell
 * (bramble-android, MessageNotifier). The webapp owns the node connection
 * for every transport (WiFi and BLE), so it forwards each incoming message
 * here and the shell renders a native MessagingStyle notification.
 */
export type BrambleAndroidNotifyApi = {
  /** payload is a JSON-encoded NativeMessageNotification. */
  onMessage(payloadJson: string): void;
  /** Clear the notification for a conversation the user has opened. */
  clearConversation(conversationId: string): void;
};

declare global {
  interface Window {
    brambleDesktop?: BrambleDesktopApi;
    brambleAndroidNative?: BrambleAndroidNativeApi;
    brambleAndroidNotify?: BrambleAndroidNotifyApi;
    /**
     * Registered by the webapp in the Android shell; the native side calls it
     * when the user taps a message notification, to open that conversation.
     */
    brambleOpenConversation?: (conversationId: string) => void;
  }
}
