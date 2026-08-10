import type { Message, DeliveryStatus, RelayHop } from '../types/bramble';

// The message cache is a flat, id-keyed per-node log, read back in timestamp
// order. Conversation bucketing is NOT persisted here: the store re-derives
// each message's bucket in memory through conversationIdForMessage
// (store/index.ts) whenever it loads the cache, so this layer never has to
// keep a denormalized bucket id in sync with the classifier the UI renders.

class MessageDb {
  private db: IDBDatabase | null = null;
  private readonly DB_VERSION = 3;
  private readonly STORE_NAME = 'messages';
  private nodeAddr: string = '';

  /** Open (or reopen) the DB for a specific node address */
  async open(nodeAddr?: string): Promise<void> {
    const addr = nodeAddr || 'default';
    // If switching nodes, close old DB
    if (this.db && this.nodeAddr !== addr) {
      this.db.close();
      this.db = null;
    }
    this.nodeAddr = addr;
    if (this.db) return;
    if (typeof indexedDB === 'undefined') return;

    const dbName = `bramble-messages-${addr}`;
    return new Promise((resolve, reject) => {
      const req = indexedDB.open(dbName, this.DB_VERSION);
      req.onupgradeneeded = () => {
        const db = req.result;
        const store = db.objectStoreNames.contains(this.STORE_NAME)
          ? req.transaction!.objectStore(this.STORE_NAME)
          : db.createObjectStore(this.STORE_NAME, { keyPath: 'id' });

        if (!store.indexNames.contains('by-timestamp')) {
          store.createIndex('by-timestamp', 'timestampMs', { unique: false });
        }
        // v1/v2 carried a by-conversation index (and a denormalized
        // conversationId column) that nothing ever queried: getMessages is only
        // ever called with no argument, reading the whole log in timestamp
        // order. Drop the index from existing databases on the v3 upgrade. Any
        // stale conversationId still on old rows is ignored on read.
        if (store.indexNames.contains('by-conversation')) {
          store.deleteIndex('by-conversation');
        }
      };
      req.onsuccess = () => {
        this.db = req.result;
        resolve();
      };
      req.onerror = () => reject(req.error);
    });
  }

  async saveMessage(msg: Message): Promise<void> {
    if (!this.db) return;
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.STORE_NAME, 'readwrite');
      tx.objectStore(this.STORE_NAME).put(msg);
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }

  async saveMessages(msgs: Message[]): Promise<void> {
    if (!this.db || msgs.length === 0) return;
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.STORE_NAME, 'readwrite');
      const store = tx.objectStore(this.STORE_NAME);
      for (const msg of msgs) {
        store.put(msg);
      }
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }

  async getMessages(): Promise<Message[]> {
    if (!this.db) return [];
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.STORE_NAME, 'readonly');
      const store = tx.objectStore(this.STORE_NAME);
      const req = store.index('by-timestamp').getAll();
      req.onsuccess = () => {
        // Strip any legacy conversationId left on rows written before v3, so
        // returned objects match the Message type whatever version wrote them.
        const rows = req.result as Array<Message & { conversationId?: string }>;
        resolve(rows.map(({ conversationId: _legacy, ...msg }) => msg));
      };
      req.onerror = () => reject(req.error);
    });
  }

  async updateMessageStatus(id: string, status: DeliveryStatus, relayPath?: RelayHop[]): Promise<void> {
    if (!this.db) return;
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.STORE_NAME, 'readwrite');
      const store = tx.objectStore(this.STORE_NAME);
      const getReq = store.get(id);
      getReq.onsuccess = () => {
        const record = getReq.result as Message | undefined;
        if (record) {
          record.status = status;
          if (relayPath) record.relayPath = relayPath;
          store.put(record);
        }
      };
      tx.oncomplete = () => resolve();
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

export const messageDb = new MessageDb();
