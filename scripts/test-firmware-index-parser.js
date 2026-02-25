#!/usr/bin/env node
const fs = require('fs');
const path = require('path');
const { normalizeAndSortReleases, resolveArtifactsForBoardRelease, normalizeVersionLabel } = require('../web-flasher/release-index.js');

const fixture = path.join(__dirname, '..', 'test', 'fixtures', 'firmware-index-valid.json');
const data = JSON.parse(fs.readFileSync(fixture, 'utf8'));
const rel = normalizeAndSortReleases(data);
if (!Array.isArray(rel) || rel.length !== 1) {
  console.error(`FAIL: expected 1 valid release, got ${Array.isArray(rel) ? rel.length : 'non-array'}`);
  process.exit(1);
}

if (rel[0].version !== 'v1.2.3') {
  console.error(`FAIL: expected normalized semver label v1.2.3, got ${rel[0].version}`);
  process.exit(1);
}

if (normalizeVersionLabel('1.2.3-beta.1') !== 'v1.2.3-beta.1') {
  console.error('FAIL: semver label normalization mismatch for prerelease');
  process.exit(1);
}

const boardCfg = {
  partitions: [
    { name: 'bootloader', file: 'bootloader.bin' },
    { name: 'partition-table', file: 'partition-table.bin' },
    { name: 'firmware', file: 'bramble.bin' }
  ]
};
const map = resolveArtifactsForBoardRelease('heltec-v3', rel[0], boardCfg);
if (!map['bootloader.bin'] || !map['partition-table.bin'] || !map['bramble.bin']) {
  console.error('FAIL: missing resolved artifact mapping');
  process.exit(1);
}

console.log('OK: parser + resolver + semver label checks passed');
