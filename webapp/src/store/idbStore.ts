// Shared plumbing for the per-node IndexedDB caches (messages and delivery
// events). Both stores are a single object store namespaced by node address,
// opened with the same close-on-node-switch, private-browsing guard, and
// transaction skeleton. This base holds that shared lifecycle so each concrete
// store only declares its store name, key path, and schema migration, plus the
// reads and writes unique to it.

export interface IdbStoreConfig {
  /** DB name prefix; the node address is appended as `${prefix}-${addr}`. */
  dbPrefix: string;
  version: number;
  storeName: string;
  keyPath: string;
  /**
   * Create or drop indexes on the object store during an upgrade. Runs against
   * either a freshly created store or the existing one, so it must be
   * idempotent (guard every createIndex/deleteIndex with an indexNames check).
   */
  migrate(store: IDBObjectStore): void;
}

export abstract class IdbStore {
  private db: IDBDatabase | null = null;
  private nodeAddr = '';
  private readonly config: IdbStoreConfig;

  protected constructor(config: IdbStoreConfig) {
    this.config = config;
  }

  /** Open (or reopen) the DB for a specific node address. */
  async open(nodeAddr?: string): Promise<void> {
    const addr = nodeAddr || 'default';
    // Switching nodes: close the old DB so the next open namespaces under addr.
    if (this.db && this.nodeAddr !== addr) {
      this.db.close();
      this.db = null;
    }
    this.nodeAddr = addr;
    if (this.db) return;
    if (typeof indexedDB === 'undefined') return;

    const dbName = `${this.config.dbPrefix}-${addr}`;
    return new Promise((resolve, reject) => {
      const req = indexedDB.open(dbName, this.config.version);
      req.onupgradeneeded = () => {
        const db = req.result;
        const store = db.objectStoreNames.contains(this.config.storeName)
          ? req.transaction!.objectStore(this.config.storeName)
          : db.createObjectStore(this.config.storeName, { keyPath: this.config.keyPath });
        this.config.migrate(store);
      };
      req.onsuccess = () => {
        this.db = req.result;
        resolve();
      };
      req.onerror = () => reject(req.error);
    });
  }

  /**
   * Run a readwrite transaction, enqueueing operations via `run`, and resolve
   * once the transaction commits. A no-op (resolves immediately) when the DB is
   * not open, matching the "persistence unavailable, degrade quietly" contract.
   */
  protected write(run: (store: IDBObjectStore) => void): Promise<void> {
    if (!this.db) return Promise.resolve();
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.config.storeName, 'readwrite');
      run(tx.objectStore(this.config.storeName));
      tx.oncomplete = () => resolve();
      tx.onerror = () => reject(tx.error);
    });
  }

  /**
   * Run a readonly request built by `run` and resolve `map(request.result)`.
   * Resolves `empty` when the DB is not open.
   */
  protected read<T>(run: (store: IDBObjectStore) => IDBRequest, map: (raw: unknown) => T, empty: T): Promise<T> {
    if (!this.db) return Promise.resolve(empty);
    return new Promise((resolve, reject) => {
      const tx = this.db!.transaction(this.config.storeName, 'readonly');
      const req = run(tx.objectStore(this.config.storeName));
      req.onsuccess = () => resolve(map(req.result));
      req.onerror = () => reject(req.error);
    });
  }

  async clearAll(): Promise<void> {
    return this.write(store => store.clear());
  }
}
