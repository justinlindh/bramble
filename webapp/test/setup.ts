import '@testing-library/jest-dom';
import { indexedDB, IDBKeyRange } from 'fake-indexeddb';

// Suppress console.debug in tests — store debug logging is noisy
const originalDebug = console.debug;
console.debug = (..._args: unknown[]) => {}; // eslint-disable-line @typescript-eslint/no-unused-vars
afterAll(() => { console.debug = originalDebug; });

(globalThis as unknown as { indexedDB: IDBFactory }).indexedDB = indexedDB;
(globalThis as unknown as { IDBKeyRange: typeof globalThis.IDBKeyRange }).IDBKeyRange = IDBKeyRange;

// Mock localStorage for tests
const localStorageMock = (() => {
  let store: Record<string, string> = {};

  return {
    getItem: (key: string) => store[key] || null,
    setItem: (key: string, value: string) => {
      store[key] = value.toString();
    },
    removeItem: (key: string) => {
      delete store[key];
    },
    clear: () => {
      store = {};
    },
  };
})();

global.localStorage = localStorageMock as Storage;
