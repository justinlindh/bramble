declare global {
  // Set by electron preload script
  // eslint-disable-next-line no-var
  var isElectron: boolean | undefined;
}

/** Returns true when running inside Electron */
export function isElectron(): boolean {
  return typeof globalThis.isElectron === 'boolean' && globalThis.isElectron;
}

/** Returns true when Web Serial API is available */
export function hasSerialSupport(): boolean {
  return typeof navigator !== 'undefined' && 'serial' in navigator;
}

/** Returns true when Web Bluetooth API is available */
export function hasBLESupport(): boolean {
  return typeof navigator !== 'undefined' && 'bluetooth' in navigator;
}

/** Returns the platform runtime name for display purposes */
export function platformName(): string {
  if (isElectron()) return 'desktop';
  if ('serviceWorker' in navigator) return 'pwa';
  return 'browser';
}
