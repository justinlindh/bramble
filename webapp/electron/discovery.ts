import { Bonjour, Browser, Service } from 'bonjour-service';
import { serviceToNode, upsertNode, removeService } from '../src/lib/discoveryCore';
import type { DiscoveredNode } from '../src/types/desktop';

// bonjour-service 1.4.3 changed its type surface from ES named exports to an
// `export =` class/namespace merge. The classes are unchanged at runtime, but
// `Bonjour`, `Browser` and `Service` now resolve as values only, so instance
// types have to be recovered with InstanceType.
type BonjourInstance = InstanceType<typeof Bonjour>;
type BrowserInstance = InstanceType<typeof Browser>;
type ServiceInstance = InstanceType<typeof Service>;

let bonjour: BonjourInstance | null = null;
let browser: BrowserInstance | null = null;
let snapshot: DiscoveredNode[] = [];

/**
 * Starts browsing _bramble._tcp and pushes a full deduped snapshot on every
 * service up/down event. Discovery is best-effort: multicast can fail on
 * VPN or firewalled networks, in which case the renderer gets an empty
 * snapshot and the UI degrades to manual IP entry.
 */
export function startDiscovery(onUpdate: (nodes: DiscoveredNode[]) => void): void {
  stopDiscovery();
  try {
    // The second argument receives async multicast socket errors (VPNs,
    // firewalls); without it they surface as uncaught exceptions in main.
    bonjour = new Bonjour(undefined, (err: Error) => {
      console.warn('[discovery] mDNS error:', err);
      stopDiscovery();
      onUpdate([]);
    });
    browser = bonjour.find({ type: 'bramble' });
  } catch (err) {
    console.warn('[discovery] mDNS unavailable:', err);
    stopDiscovery();
    onUpdate([]);
    return;
  }
  browser.on('up', (svc: ServiceInstance) => {
    const node = serviceToNode(svc);
    if (!node) return;
    snapshot = upsertNode(snapshot, node);
    onUpdate(snapshot);
  });
  browser.on('down', (svc: ServiceInstance) => {
    snapshot = removeService(snapshot, svc);
    onUpdate(snapshot);
  });
  onUpdate(snapshot);
}

export function stopDiscovery(): void {
  try { browser?.stop(); } catch { /* noop */ }
  try { bonjour?.destroy(); } catch { /* noop */ }
  browser = null;
  bonjour = null;
  snapshot = [];
}
