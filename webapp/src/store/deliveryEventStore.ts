import { IdbStore } from './idbStore';

export interface DeliveryEventRecord {
  eventId: string;
  messageId: string;
  packetId?: string;
  ts: number;
  eventType: string;
  payload?: unknown;
}

const STORE_NAME = 'delivery_events';

function byTs(events: DeliveryEventRecord[]): DeliveryEventRecord[] {
  return events.sort((a, b) => a.ts - b.ts);
}

class DeliveryEventStore extends IdbStore {
  constructor() {
    super({
      dbPrefix: 'bramble-delivery-events',
      version: 3,
      storeName: STORE_NAME,
      keyPath: 'eventId',
      migrate(store) {
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
      },
    });
  }

  async upsertDeliveryEvent(event: DeliveryEventRecord): Promise<void> {
    return this.write(store => store.put(event));
  }

  async upsertDeliveryEvents(events: DeliveryEventRecord[]): Promise<void> {
    if (events.length === 0) return;
    return this.write(store => {
      for (const event of events) {
        store.put(event);
      }
    });
  }

  async listByMessage(messageId: string): Promise<DeliveryEventRecord[]> {
    return this.read(
      store => store.index('by-message').getAll(messageId),
      raw => byTs(raw as DeliveryEventRecord[]),
      [],
    );
  }

  async listByPacketId(packetId: string): Promise<DeliveryEventRecord[]> {
    return this.read(
      store => store.index('by-packet').getAll(packetId),
      raw => byTs(raw as DeliveryEventRecord[]),
      [],
    );
  }

  async pruneOldEvents(cutoffTs: number): Promise<number> {
    let deleted = 0;
    await this.write(store => {
      const range = IDBKeyRange.upperBound(cutoffTs, true);
      const req = store.index('by-ts').openCursor(range);
      req.onsuccess = () => {
        const cursor = req.result;
        if (!cursor) return;
        cursor.delete();
        deleted += 1;
        cursor.continue();
      };
    });
    return deleted;
  }
}

export const deliveryEventStore = new DeliveryEventStore();
