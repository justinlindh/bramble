import { defineConfig } from 'vitest/config';
import react from '@vitejs/plugin-react';
import { execSync } from 'node:child_process';
import { readFileSync } from 'node:fs';

const pkg = JSON.parse(readFileSync('./package.json', 'utf-8'));
const gitHash = (() => {
  try { return execSync('git rev-parse --short HEAD', { encoding: 'utf-8' }).trim(); }
  catch { return 'unknown'; }
})();

export default defineConfig({
  base: './',
  plugins: [react()],
  define: {
    __APP_VERSION__: JSON.stringify(`${pkg.version}+${gitHash}`),
  },
  test: {
    environment: 'jsdom',
    setupFiles: ['./test/setup.ts'],
    globals: true,
  },
  build: {
    outDir: 'dist',
    sourcemap: true,
  },
});
