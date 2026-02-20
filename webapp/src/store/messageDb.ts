import type { Message, DeliveryStatus, RelayHop } from '../types/bramble';

type DbMessage = Message & { conversationId: string };

function computeConversationId(msg: Message): string {
  if (msg.to === 0xFFFFFFFF || msg.channelIndex === -1) return 'broadcast';
  if (msg.channelIndex !== undefined && msg.channelIndex >= 0) return `ch:${msg.channelIndex}`;
  const a = Math.min(msg.from, msg.to);
  const b = Math.max(msg.from, msg.to);
  return `dm:${a.toString(16)}-${b.toString(16)}`;
}

class MessageDb {
  private db: IDBDatabase | null = null;
  private readonly DB_VERSION = 1;
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
        if (!db.objectStoreNames.contains(this.STORE_NAME)) {
          const store = db.createObjectStore(this.STORE_NAME, { keyPath: 'id' });
          store.createIndex('by-conversation', 'conversationId', { unique: false });
          store.createIndex('by-timestamp', 'timestampMs', { unique: false });
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
    const record: DbMessage = { ...msg, conversationId: computeConversationId(msg) };
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
        store.put({ ...msg, conversationId: computeConversationId(msg) });
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
