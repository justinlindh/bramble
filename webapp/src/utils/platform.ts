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

/**
 * Which kind of device the browser is running on. `android` means any
 * non-iOS mobile browser; in practice that is Chrome on Android, and the
 * copy keyed on it says so. The distinction that matters is which fixes are
 * reachable: a desktop OS can install the desktop app, a phone cannot, and
 * iOS additionally has no Web Bluetooth and no native app.
 */
export type Platform = 'desktop' | 'android' | 'ios';

interface UserAgentDataLike {
  mobile?: boolean;
}

export function describePlatform(nav: Navigator = navigator): Platform {
  const ua = nav.userAgent || '';
  // iPadOS 13+ reports a Macintosh user agent, so touch points are the only
  // reliable way to tell an iPad from a Mac.
  const isIOS = /iPhone|iPod|iPad/.test(ua) || (/Macintosh/.test(ua) && (nav.maxTouchPoints ?? 0) > 1);
  if (isIOS) return 'ios';
  if (/Android/.test(ua)) return 'android';
  const uaData = (nav as Navigator & { userAgentData?: UserAgentDataLike }).userAgentData;
  if (uaData?.mobile === true) return 'android';
  return 'desktop';
}
