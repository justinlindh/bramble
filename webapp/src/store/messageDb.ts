import type { Message, DeliveryStatus, RelayHop } from '../types/bramble';

type DbMessage = Message & { conversationId: string };

/**
 * Compute conversation ID matching the Zustand store convention:
 *   broadcast → 'broadcast'
 *   channel   → 'ch:{index}'
 *   DM        → 'dm:{peerAddr}' (the other party's address)
 *
 * For DMs we need the local node address to determine which side is "us".
 * If not provided, falls back to the outgoing/incoming direction heuristic.
 */
function computeConversationId(msg: Message, selfAddr?: number): string {
  if (msg.to === 0xFFFFFFFF || msg.channelIndex === -1) return 'broadcast';
  if (msg.channelIndex !== undefined && msg.channelIndex >= 0) return `ch:${msg.channelIndex}`;
  // DM: key by the peer's address (not ours)
  if (selfAddr !== undefined) {
    const peerAddr = msg.from === selfAddr ? msg.to : msg.from;
    return `dm:${peerAddr}`;
  }
  // Fallback: use direction if available, otherwise use the "to" address
  const peerAddr = msg.direction === 'outgoing' ? msg.to : msg.from;
  return `dm:${peerAddr}`;
}

class MessageDb {
  private db: IDBDatabase | null = null;
  private readonly DB_VERSION = 2;
  private readonly STORE_NAME = 'messages';
  private nodeAddr: string = '';
  private selfAddrNum: number | undefined;

  /** Open (or reopen) the DB for a specific node address */
  async open(nodeAddr?: string): Promise<void> {
    const addr = nodeAddr || 'default';
    // Parse numeric address for conversation ID computation
    this.selfAddrNum = addr !== 'default' ? parseInt(addr, 16) : undefined;
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
      req.onupgradeneeded = (event) => {
        const db = req.result;
        const oldVersion = event.oldVersion;
        if (oldVersion < 1) {
          // Fresh DB — create store with indexes
          const store = db.createObjectStore(this.STORE_NAME, { keyPath: 'id' });
          store.createIndex('by-conversation', 'conversationId', { unique: false });
          store.createIndex('by-timestamp', 'timestampMs', { unique: false });
        }
        if (oldVersion >= 1 && oldVersion < 2) {
          // v1 → v2: conversation IDs changed from dm:{min}-{max} to dm:{peerAddr}.
          // We can't rewrite keys during upgrade (no selfAddr yet), so we'll
          // re-index on first read. The index itself doesn't need schema changes.
        }
      };
      req.onsuccess = () => {
        this.db = req.result;
        // Migrate old v1 conversation IDs in background
        this.migrateV1ConversationIds().catch(() => {});
        resolve();
      };
      req.onerror = () => reject(req.error);
    });
  }

  /**
   * Re-key any DM conversation IDs from the old v1 format (dm:{min}-{max})
   * to the current format (dm:{peerAddr}).
   */
  private async migrateV1ConversationIds(): Promise<void> {
    if (!this.db || this.selfAddrNum === undefined) return;
    const migratedKey = `bramble:msgdb-migrated-v2:${this.nodeAddr}`;
    try {
      if (localStorage.getItem(migratedKey)) return;
    } catch { /* proceed anyway */ }

    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.STORE_NAME, 'readwrite');
      const store = tx.objectStore(this.STORE_NAME);
      const req = store.openCursor();
      req.onsuccess = () => {
        const cursor = req.result;
        if (!cursor) return;
        const record = cursor.value as DbMessage;
        // Detect old format: dm:{hex}-{hex}
        if (record.conversationId && /^dm:[0-9a-f]+-[0-9a-f]+$/i.test(record.conversationId)) {
          const newId = computeConversationId(record, this.selfAddrNum);
          if (newId !== record.conversationId) {
            cursor.update({ ...record, conversationId: newId });
          }
        }
        cursor.continue();
      };
      tx.oncomplete = () => {
        try { localStorage.setItem(migratedKey, '1'); } catch {}
        resolve();
      };
      tx.onerror = () => reject(tx.error);
    });
  }

  async saveMessage(msg: Message): Promise<void> {
    if (!this.db) return;
    const record: DbMessage = { ...msg, conversationId: computeConversationId(msg, this.selfAddrNum) };
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.STORE_NAME, 'readwrite');
      tx.objectStore(this.STORE_NAME).put(record);
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
        store.put({ ...msg, conversationId: computeConversationId(msg, this.selfAddrNum) });
      }
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }

  async getMessages(conversationId?: string): Promise<Message[]> {
    if (!this.db) return [];
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.STORE_NAME, 'readonly');
      const store = tx.objectStore(this.STORE_NAME);
      let req: IDBRequest;
      if (conversationId) {
        req = store.index('by-conversation').getAll(conversationId);
      } else {
        req = store.index('by-timestamp').getAll();
      }
      req.onsuccess = () => {
        const results = (req.result as DbMessage[]).map(({ conversationId: _, ...msg }) => msg as Message);
        resolve(results);
      };
      req.onerror = () => reject(req.error);
    });
  }

  async getLastSyncTimestamp(): Promise<number> {
    if (!this.db) return 0;
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.STORE_NAME, 'readonly');
      const index = tx.objectStore(this.STORE_NAME).index('by-timestamp');
      const req = index.openCursor(null, 'prev');
      req.onsuccess = () => {
        const cursor = req.result;
        resolve(cursor ? (cursor.value as DbMessage).timestampMs : 0);
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
        const record = getReq.result as DbMessage | undefined;
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

  async deleteConversation(conversationId: string): Promise<void> {
    if (!this.db) return;
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.STORE_NAME, 'readwrite');
      const store = tx.objectStore(this.STORE_NAME);
      const index = store.index('by-conversation');
      const req = index.openCursor(IDBKeyRange.only(conversationId));
      req.onsuccess = () => {
        const cursor = req.result;
        if (cursor) {
          cursor.delete();
          cursor.continue();
        }
      };
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }
}

export const messageDb = new MessageDb();
