#!/usr/bin/env node
const fs = require('fs');
const path = require('path');
const { normalizeAndSortReleases, resolveArtifactsForBoardRelease } = require('../web-flasher/release-index.js');

const fixture = path.join(__dirname, '..', 'test', 'fixtures', 'firmware-index-valid.json');
const data = JSON.parse(fs.readFileSync(fixture, 'utf8'));
const rel = normalizeAndSortReleases(data);
if (!Array.isArray(rel) || rel.length < 1) {
  console.error('FAIL: expected at least one release');
  process.exit(1);
}

const boardCfg = {
  partitions: [
    { name: 'firmware', file: 'bramble-heltec.bin' }
  ]
};
const map = resolveArtifactsForBoardRelease('heltec-v3', rel[0], boardCfg);
if (!map['bramble-heltec.bin']) {
  console.error('FAIL: missing resolved artifact mapping');
  process.exit(1);
}

console.log('OK: parser + resolver smoke test passed');
