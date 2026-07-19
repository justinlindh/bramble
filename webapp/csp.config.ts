/**
 * Content-Security-Policy for the renderer, shared by the browser build
 * (vite.config.ts) and the Electron renderer build (electron.vite.config.ts).
 *
 * Delivered as a <meta http-equiv> tag injected at build time rather than as a
 * response header. The packaged desktop app loads the renderer with
 * BrowserWindow.loadFile(), and session.webRequest.onHeadersReceived never
 * fires for file:// requests, so a header-based policy would silently protect
 * nothing in exactly the build that needs it most. A meta tag travels with the
 * document and therefore covers all three deployments: file:// in Electron,
 * the hosted webapp, and the Android WebView shell.
 *
 * frame-ancestors is deliberately absent: it is ignored when a policy arrives
 * via meta tag, so it belongs on the hosting server, not here.
 */

/** Where the Map page fetches its raster tiles from (see src/pages/Map/Map.tsx). */
const TILE_HOST = 'https://*.tile.openstreetmap.org';

/**
 * connect-src has to stay broad. The WiFi transport opens a socket at
 * ws://<node-ip>/ws for an address the user types in, the hosted proxy uses
 * wss://<host>/proxy/<ip>, and the OTA release index is fetched from a
 * user-configured origin over http or https. None of those are known at build
 * time, so the directive cannot be narrowed to a fixed host list. The value is
 * still real: it does not widen script-src, which is the directive that
 * actually contains an injection.
 */
const CONNECT_SRC = "'self' ws: wss: http: https:";

/**
 * style-src keeps 'unsafe-inline'. The dev server injects CSS through
 * runtime-created <style> elements, and Leaflet writes inline style attributes
 * onto the panes and markers it creates. Both are style-only vectors.
 */
const STYLE_SRC = "'self' 'unsafe-inline'";

/**
 * img-src allows data: because Vite inlines the Leaflet marker and shadow
 * assets under its 4 KB threshold, so they arrive as data:image/png and
 * data:image/gif URIs inside the Map chunk.
 */
const IMG_SRC = `'self' data: ${TILE_HOST}`;

const BASE_DIRECTIVES = [
  "default-src 'self'",
  "base-uri 'self'",
  "object-src 'none'",
  "frame-src 'none'",
  "form-action 'none'",
  "font-src 'self'",
  `img-src ${IMG_SRC}`,
  `style-src ${STYLE_SRC}`,
  `connect-src ${CONNECT_SRC}`,
];

/** Production: no inline or eval'd script of any kind. */
export const PRODUCTION_CSP = [...BASE_DIRECTIVES, "script-src 'self'"].join('; ');

/**
 * Development: Vite injects the React Fast Refresh preamble as an inline
 * module script into index.html and evaluates transformed modules, so the dev
 * policy has to permit inline and eval script. This looseness is scoped to
 * `vite`/`electron-vite dev` and never reaches a built artifact.
 */
export const DEVELOPMENT_CSP = [
  ...BASE_DIRECTIVES,
  "script-src 'self' 'unsafe-inline' 'unsafe-eval'",
].join('; ');

/**
 * Vite plugin that injects the policy into index.html. `command` is 'serve'
 * for the dev server and 'build' for a production build, which is what picks
 * between the two policies.
 */
export function cspPlugin() {
  let isDev = false;
  return {
    name: 'bramble-csp',
    configResolved(config: { command: string }) {
      isDev = config.command === 'serve';
    },
    transformIndexHtml() {
      return [
        {
          tag: 'meta',
          attrs: {
            'http-equiv': 'Content-Security-Policy',
            content: isDev ? DEVELOPMENT_CSP : PRODUCTION_CSP,
          },
          injectTo: 'head-prepend' as const,
        },
      ];
    },
  };
}
