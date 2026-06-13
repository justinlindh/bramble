import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';

// Target for the dev-server API proxy. Override with VITE_API_PROXY_TARGET if
// your local unified server runs on a different port.
const API_PROXY_TARGET = process.env.VITE_API_PROXY_TARGET ?? 'http://localhost:8085';

export default defineConfig({
  base: './',
  plugins: [react()],
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
    exclude: ['**/node_modules/**', '**/test/integration/**', '**/web-flasher/**'],
  },
  build: {
    outDir: 'dist',
    sourcemap: true,
  },
});
