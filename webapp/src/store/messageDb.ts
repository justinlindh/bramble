import type { Message, DeliveryStatus, RelayHop } from '../types/bramble';
import { IdbStore } from './idbStore';

// The message cache is a flat, id-keyed per-node log, read back in timestamp
// order. Conversation bucketing is NOT persisted here: the store re-derives
// each message's bucket in memory through conversationIdForMessage
// (store/index.ts) whenever it loads the cache, so this layer never has to
// keep a denormalized bucket id in sync with the classifier the UI renders.

const STORE_NAME = 'messages';

class MessageDb extends IdbStore {
  constructor() {
    super({
      dbPrefix: 'bramble-messages',
      version: 3,
      storeName: STORE_NAME,
      keyPath: 'id',
      migrate(store) {
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
      },
    });
  }

  async saveMessage(msg: Message): Promise<void> {
    return this.write(store => store.put(msg));
  }

  async saveMessages(msgs: Message[]): Promise<void> {
    if (msgs.length === 0) return;
    return this.write(store => {
      for (const msg of msgs) {
        store.put(msg);
      }
    });
  }

  async getMessages(): Promise<Message[]> {
    return this.read(
      store => store.index('by-timestamp').getAll(),
      raw => {
        // Strip any legacy conversationId left on rows written before v3, so
        // returned objects match the Message type whatever version wrote them.
        const rows = raw as Array<Message & { conversationId?: string }>;
        return rows.map(({ conversationId: _legacy, ...msg }) => msg);
      },
      [],
    );
  }

  async updateMessageStatus(id: string, status: DeliveryStatus, relayPath?: RelayHop[]): Promise<void> {
    return this.write(store => {
      const getReq = store.get(id);
      getReq.onsuccess = () => {
        const record = getReq.result as Message | undefined;
        if (record) {
          record.status = status;
          if (relayPath) record.relayPath = relayPath;
          store.put(record);
        }
      };
    });
  }
}

export const messageDb = new MessageDb();
