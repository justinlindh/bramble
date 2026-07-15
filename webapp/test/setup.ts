import '@testing-library/jest-dom';
import { indexedDB, IDBKeyRange } from 'fake-indexeddb';

// Suppress console.debug in tests: store debug logging is noisy.
const originalDebug = console.debug;
console.debug = (..._args: unknown[]) => {}; // eslint-disable-line @typescript-eslint/no-unused-vars
afterAll(() => { console.debug = originalDebug; });

// actions.ts keeps module-level singletons (the transport client, the
// packet/broadcast id correlation maps, the pending sent-status timers).
// Reset them after every test so one test's connect()/sendMessage() state
// cannot leak into the next test file. Imported dynamically (not at this
// file's top level): a static import here would load the real actions.ts
// (and its real ../transport import) before a test file's own hoisted
// vi.mock('../../transport', ...) gets a chance to intercept it, silently
// un-mocking every connect()-driving test in the suite.
afterEach(async () => {
  // Some test files fully mock '../../store/actions' (or '../actions') and
  // don't re-export this test hook. Vitest's mock proxy throws on ANY access
  // to a property the mock factory didn't return (not just on call), so this
  // has to be a try/catch, not optional chaining: those files have nothing
  // real to reset here anyway.
  try {
    const mod = await import('../src/store/actions') as { __resetActionsForTests?: () => void };
    mod.__resetActionsForTests?.();
  } catch {
    // no-op: module is fully mocked without this test hook
  }
});

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
