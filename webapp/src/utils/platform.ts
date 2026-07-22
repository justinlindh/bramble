declare global {
  // Set by electron preload script
  // eslint-disable-next-line no-var
  var isElectron: boolean | undefined;
  // Set by the Android WebView shell (bramble-android) before page load
  // eslint-disable-next-line no-var
  var brambleAndroid: boolean | undefined;
}

/** Returns true when running inside Electron */
export function isElectron(): boolean {
  return typeof globalThis.isElectron === 'boolean' && globalThis.isElectron;
}

/** Returns true when running inside the Android WebView shell */
export function isAndroidShell(): boolean {
  return typeof globalThis.brambleAndroid === 'boolean' && globalThis.brambleAndroid;
}

/**
 * Returns true in any embedded shell (Electron, Android WebView). Embedded
 * shells load the app from a local origin with no unified server behind it
 * and may open ws:// LAN sockets directly, so capabilities and WiFi URL
 * building must not depend on the page origin.
 */
export function isEmbeddedShell(): boolean {
  return isElectron() || isAndroidShell();
}
