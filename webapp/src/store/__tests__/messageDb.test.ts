import { beforeEach, describe, expect, it } from 'vitest';
import { messageDb } from '../messageDb';
import type { Message } from '../../types/bramble';

// messageDb is a flat, id-keyed per-node log. It does not persist conversation
// buckets: the store re-derives those in memory on load. These tests pin the
// surviving contract: messages round-trip, are read back in timestamp order,
// update in place, and clear.

function msg(over: Partial<Message>): Message {
  return {
    id: Math.random().toString(36).slice(2),
    direction: 'incoming',
    from: 0xa1b2c3d4,
    to: 0x11111111,
    text: 'hi',
    timestampMs: 1000,
    tier: 'normal',
    status: 'delivered',
    ...over,
  };
}

describe('messageDb persistence', () => {
  beforeEach(async () => {
    await messageDb.open('11111111');
    await messageDb.clearAll();
  });

  it('round-trips saved messages in timestamp order', async () => {
    const a = msg({ timestampMs: 3000 });
    const b = msg({ timestampMs: 1000 });
    const c = msg({ timestampMs: 2000 });
    await messageDb.saveMessages([a, b, c]);

    const got = await messageDb.getMessages();
    // by-timestamp index yields ascending order regardless of insert order.
    expect(got.map(m => m.timestampMs)).toEqual([1000, 2000, 3000]);
    expect(got.map(m => m.id).sort()).toEqual([a.id, b.id, c.id].sort());
  });

  it('does not leak a persisted conversationId onto returned messages', async () => {
    const m = msg({ direction: 'outgoing', from: 0, to: 0xa1b2c3d4 });
    await messageDb.saveMessage(m);

    const [got] = await messageDb.getMessages();
    expect(got).not.toHaveProperty('conversationId');
  });

  it('updates a stored message status in place', async () => {
    const m = msg({ status: 'sending' });
    await messageDb.saveMessage(m);

    await messageDb.updateMessageStatus(m.id, 'delivered', [{ addr: 0x2222, rssi: -80 }]);

    const [got] = await messageDb.getMessages();
    expect(got.status).toBe('delivered');
    expect(got.relayPath).toEqual([{ addr: 0x2222, rssi: -80 }]);
  });

  it('clears the log', async () => {
    await messageDb.saveMessages([msg({}), msg({})]);
    await messageDb.clearAll();
    expect(await messageDb.getMessages()).toHaveLength(0);
  });

  // Pre-build a v2 database (the old schema: a by-conversation index and a
  // denormalized conversationId column) and confirm the v3 open upgrades it in
  // place: the dead index is dropped, existing rows survive, and the stale
  // conversationId does not leak back out.
  it('upgrades a legacy v2 database by dropping the dead by-conversation index', async () => {
    const addr = 'abcd0002';
    const dbName = `bramble-messages-${addr}`;
    const legacyId = 'legacy-row';

    await new Promise<void>((resolve, reject) => {
      const req = indexedDB.open(dbName, 2);
      req.onupgradeneeded = () => {
        const store = req.result.createObjectStore('messages', { keyPath: 'id' });
        store.createIndex('by-conversation', 'conversationId', { unique: false });
        store.createIndex('by-timestamp', 'timestampMs', { unique: false });
      };
      req.onsuccess = () => {
        const db = req.result;
        const tx = db.transaction('messages', 'readwrite');
        tx.objectStore('messages').put({ ...msg({ id: legacyId }), conversationId: 'dm:deadbeef-cafe' });
        tx.oncomplete = () => { db.close(); resolve(); };
        tx.onerror = () => reject(tx.error);
      };
      req.onerror = () => reject(req.error);
    });

    // The v3 open must run the upgrade without error and keep the row.
    await messageDb.open(addr);
    const got = await messageDb.getMessages();
    expect(got.map(m => m.id)).toContain(legacyId);
    expect(got.find(m => m.id === legacyId)).not.toHaveProperty('conversationId');

    // The dead index is gone from the upgraded schema.
    const indexNames = await new Promise<string[]>((resolve, reject) => {
      const req = indexedDB.open(dbName);
      req.onsuccess = () => {
        const db = req.result;
        const names = Array.from(db.transaction('messages', 'readonly').objectStore('messages').indexNames);
        db.close();
        resolve(names);
      };
      req.onerror = () => reject(req.error);
    });
    expect(indexNames).toContain('by-timestamp');
    expect(indexNames).not.toContain('by-conversation');
  });
});
