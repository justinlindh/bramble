import test from 'node:test';
import assert from 'node:assert/strict';
import { createRequire } from 'node:module';

const require = createRequire(import.meta.url);
const { resolveArtifactsForBoardRelease } = require('./release-index.js');

const RELEASE = {
  version: 'v1.0.0',
  artifacts: [
    { board: 'tdeck-plus', file: '/ota/stable/v1.0.0/tdeck-plus/bootloader.bin', sha256: '', size: 1 },
    { board: 'tdeck-plus', file: '/ota/stable/v1.0.0/tdeck-plus/partition-table.bin', sha256: '', size: 1 },
    { board: 'tdeck-plus', file: '/ota/stable/v1.0.0/tdeck-plus/bramble.bin', sha256: '', size: 1 },
  ],
};

test('resolveArtifactsForBoardRelease skips blank (synthesized) partitions', () => {
  const boardConfig = {
    partitions: [
      { name: 'bootloader', offset: 0x0, file: 'bootloader.bin' },
      { name: 'partition-table', offset: 0x8000, file: 'partition-table.bin' },
      { name: 'ota-data', offset: 0xe000, blank: 0x2000 },
      { name: 'firmware', offset: 0x10000, file: 'bramble.bin' },
    ],
  };
  const resolved = resolveArtifactsForBoardRelease('tdeck-plus', RELEASE, boardConfig);
  assert.equal(Object.keys(resolved).length, 3);
  assert.ok(resolved['bootloader.bin']);
  assert.ok(resolved['partition-table.bin']);
  assert.ok(resolved['bramble.bin']);
});

test('resolveArtifactsForBoardRelease still rejects a release missing a real file', () => {
  const boardConfig = {
    partitions: [
      { name: 'bootloader', offset: 0x0, file: 'bootloader.bin' },
      { name: 'ota-data', offset: 0xe000, blank: 0x2000 },
      { name: 'firmware', offset: 0x10000, file: 'missing.bin' },
    ],
  };
  assert.throws(
    () => resolveArtifactsForBoardRelease('tdeck-plus', RELEASE, boardConfig),
    /missing missing\.bin/
  );
});
