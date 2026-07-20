import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';
import { cspPlugin } from './csp.config';

// Target for the dev-server API proxy. Override with VITE_API_PROXY_TARGET if
// your local unified server runs on a different port.
const API_PROXY_TARGET = process.env.VITE_API_PROXY_TARGET ?? 'http://localhost:8085';

export default defineConfig({
  base: './',
  plugins: [react(), cspPlugin()],
  define: {
    __APP_VERSION__: JSON.stringify(process.env.APP_VERSION || 'dev'),
  },
  server: {
    // Proxy API and WebSocket paths to a local unified-server instance so that
    // `npm run dev` can exercise the WiFi transport and local-mode capabilities
    // without the fetch falling back to the hosted-mode defaults.
    //
    // Start the backend first:
    //   MODE=local node server/unified-server.mjs
    // Then in a separate terminal:
    //   npm run dev
    //
    // The proxy forwards /api/capabilities (and /ws*) to the unified server so
    // the client receives real capability data instead of the hosted-mode defaults.
    proxy: {
      '/api': {
        target: API_PROXY_TARGET,
        changeOrigin: true,
      },
      '/ws': {
        target: API_PROXY_TARGET.replace(/^http/, 'ws'),
        ws: true,
        changeOrigin: true,
      },
    },
  },
  test: {
    environment: 'jsdom',
    setupFiles: ['./test/setup.ts'],
    globals: true,
    // Mocks (vi.fn/vi.spyOn) are restored to their original implementation
    // after every test. Without this, a mock set up in one test file can
    // keep answering calls made by a later, unrelated test.
    restoreMocks: true,
    exclude: ['**/node_modules/**', '**/test/integration/**', '**/web-flasher/**'],
    // Coverage is only collected with `vitest run --coverage` (the CI ratchet
    // gate); it does not affect a plain `vitest run`. Scope it to product
    // source under src/, and emit json-summary so scripts/ci/check_coverage.py
    // can read the line percentage from coverage/coverage-summary.json.
    coverage: {
      provider: 'v8',
      reporter: ['text-summary', 'json-summary'],
      reportsDirectory: './coverage',
      include: ['src/**/*.{ts,tsx}'],
      exclude: ['src/**/*.d.ts', 'src/**/*.test.{ts,tsx}', 'src/main.tsx'],
    },
  },
  build: {
    outDir: 'dist',
    // Off by default. Building with sourcemaps emitted roughly 2.5 MB of maps
    // beside 660 KB of code, and every one of them published the complete
    // unminified TypeScript client, transport and auth logic included, to
    // anyone who could reach the hosted build.
    //
    // 'hidden' was the other candidate: it still writes the maps but strips
    // the //# sourceMappingURL comment. That only helps if something uploads
    // the maps to an error reporter and then deletes them, and Bramble has no
    // error reporting service, so 'hidden' would have kept shipping the files
    // while making them marginally harder to find. Opt-in is the honest shape.
    //
    // Set SOURCEMAP=1 to get them back for a local debugging session.
    sourcemap: process.env.SOURCEMAP === '1',
  },
});
