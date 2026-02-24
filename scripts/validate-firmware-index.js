#!/usr/bin/env node
const fs = require('fs');

function fail(msg) {
  console.error(`ERROR: ${msg}`);
  process.exit(1);
}

const file = process.argv[2];
if (!file) fail('Usage: node scripts/validate-firmware-index.js <index.json>');
if (!fs.existsSync(file)) fail(`File not found: ${file}`);

let data;
try {
  data = JSON.parse(fs.readFileSync(file, 'utf8'));
} catch (e) {
  fail(`Invalid JSON: ${e.message}`);
}

if (!Array.isArray(data.releases)) fail('Top-level "releases" must be an array');

const errs = [];
data.releases.forEach((rel, i) => {
  const p = `releases[${i}]`;
  for (const k of ['version', 'published_at', 'channel', 'artifacts']) {
    if (!(k in rel)) errs.push(`${p}.${k} missing`);
  }
  if (!Array.isArray(rel.artifacts)) errs.push(`${p}.artifacts must be array`);
  (rel.artifacts || []).forEach((a, j) => {
    const ap = `${p}.artifacts[${j}]`;
    for (const k of ['board', 'file', 'sha256', 'size']) {
      if (!(k in a)) errs.push(`${ap}.${k} missing`);
    }
    if (a.sha256 && !/^[a-f0-9]{64}$/i.test(a.sha256)) errs.push(`${ap}.sha256 invalid`);
    if (a.size != null && !(Number.isInteger(a.size) && a.size > 0)) errs.push(`${ap}.size must be positive integer`);
  });
});

if (errs.length) {
  console.error('Validation failed:');
  errs.forEach(e => console.error(`- ${e}`));
  process.exit(1);
}

console.log(`OK: ${data.releases.length} release(s) validated`);
