export interface DeliveryEventRecord {
  eventId: string;
  messageId: string;
  packetId?: string;
  ts: number;
  eventType: string;
  payload?: unknown;
}

class DeliveryEventStore {
  private db: IDBDatabase | null = null;
  private readonly DB_VERSION = 3;
  private readonly STORE_NAME = 'delivery_events';
  private nodeAddr = '';

  async open(nodeAddr?: string): Promise<void> {
    const addr = nodeAddr || 'default';
    if (this.db && this.nodeAddr !== addr) {
      this.db.close();
      this.db = null;
    }
    this.nodeAddr = addr;
    if (this.db) return;
    if (typeof indexedDB === 'undefined') return;

    const dbName = `bramble-delivery-events-${addr}`;
    return new Promise((resolve, reject) => {
      const req = indexedDB.open(dbName, this.DB_VERSION);
      req.onupgradeneeded = () => {
        const db = req.result;
        const store = db.objectStoreNames.contains(this.STORE_NAME)
          ? req.transaction!.objectStore(this.STORE_NAME)
          : db.createObjectStore(this.STORE_NAME, { keyPath: 'eventId' });

        if (!store.indexNames.contains('by-message')) {
          store.createIndex('by-message', 'messageId', { unique: false });
        }
        if (!store.indexNames.contains('by-packet')) {
          store.createIndex('by-packet', 'packetId', { unique: false });
        }
        if (!store.indexNames.contains('by-ts')) {
          store.createIndex('by-ts', 'ts', { unique: false });
        }
        // v2 also created by-conversation and by-node-addr indexes that nothing
        // ever queried; drop them from existing databases on the v3 upgrade.
        if (store.indexNames.contains('by-conversation')) {
          store.deleteIndex('by-conversation');
        }
        if (store.indexNames.contains('by-node-addr')) {
          store.deleteIndex('by-node-addr');
        }
      };
      req.onsuccess = () => {
        this.db = req.result;
        resolve();
      };
      req.onerror = () => reject(req.error);
    });
  }

  async upsertDeliveryEvent(event: DeliveryEventRecord): Promise<void> {
    if (!this.db) return;
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.STORE_NAME, 'readwrite');
      tx.objectStore(this.STORE_NAME).put(event);
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }

  async upsertDeliveryEvents(events: DeliveryEventRecord[]): Promise<void> {
    if (!this.db || events.length === 0) return;
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.STORE_NAME, 'readwrite');
      const store = tx.objectStore(this.STORE_NAME);
      for (const event of events) {
        store.put(event);
      }
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }

  async listByMessage(messageId: string): Promise<DeliveryEventRecord[]> {
    if (!this.db) return [];
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.STORE_NAME, 'readonly');
      const req = tx.objectStore(this.STORE_NAME).index('by-message').getAll(messageId);
      req.onsuccess = () => {
        const events = (req.result as DeliveryEventRecord[]).sort((a, b) => a.ts - b.ts);
        resolve(events);
      };
      req.onerror = () => reject(req.error);
    });
  }

  async listByPacketId(packetId: string): Promise<DeliveryEventRecord[]> {
    if (!this.db) return [];
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.STORE_NAME, 'readonly');
      const req = tx.objectStore(this.STORE_NAME).index('by-packet').getAll(packetId);
      req.onsuccess = () => {
        const events = (req.result as DeliveryEventRecord[]).sort((a, b) => a.ts - b.ts);
        resolve(events);
      };
      req.onerror = () => reject(req.error);
    });
  }

  async pruneOldEvents(cutoffTs: number): Promise<number> {
    if (!this.db) return 0;
    return new Promise((resolve, reject) => {
      let deleted = 0;
      const tx = this.db!.transaction(this.STORE_NAME, 'readwrite');
      const index = tx.objectStore(this.STORE_NAME).index('by-ts');
      const range = IDBKeyRange.upperBound(cutoffTs, true);
      const req = index.openCursor(range);

      req.onsuccess = () => {
        const cursor = req.result;
        if (!cursor) return;
        cursor.delete();
        deleted += 1;
        cursor.continue();
      };

      tx.oncomplete = () => resolve(deleted);
      tx.onerror = () => reject(tx.error);
    });
  }

  async clearAll(): Promise<void> {
    if (!this.db) return;
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.STORE_NAME, 'readwrite');
      tx.objectStore(this.STORE_NAME).clear();
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }
}

export const deliveryEventStore = new DeliveryEventStore();
