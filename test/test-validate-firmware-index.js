#!/usr/bin/env node
// Guards scripts/validate-firmware-index.js against the OTA release-index
// schema in docs/ota-release-schema.md.
// Run with: node test/test-validate-firmware-index.js
//
// The validator is the gate scripts/publish-firmware-release.sh runs on the
// index.json it generates, so a schema regression here would let a malformed
// index reach the OTA journey that firmware updates consume. These cases pin
// the two outcomes that matter: a schema-complete index passes, and an index
// missing required fields fails and names them.
const { spawnSync } = require('node:child_process');
const path = require('node:path');

const VALIDATOR = path.join(__dirname, '..', 'scripts', 'validate-firmware-index.js');
const FIXTURES = path.join(__dirname, 'fixtures');

let failures = 0;

function run(fixture) {
  return spawnSync(process.execPath, [VALIDATOR, path.join(FIXTURES, fixture)], {
    encoding: 'utf8',
  });
}

function check(name, cond) {
  if (cond) {
    console.log(`PASS ${name}`);
  } else {
    console.error(`FAIL ${name}`);
    failures += 1;
  }
}

// A schema-complete index (including a non-semver version string, which the
// schema permits) validates and exits 0.
{
  const r = run('firmware-index-valid.json');
  check('valid index exits 0', r.status === 0);
  check('valid index reports the release count', /2 release\(s\) validated/.test(r.stdout));
}

// An index missing required per-release and per-artifact fields fails, exits
// non-zero, and names the missing fields rather than failing silently.
{
  const r = run('firmware-index-invalid.json');
  check('invalid index exits non-zero', r.status !== 0);
  check('invalid index names the missing release fields', /published_at missing/.test(r.stderr) && /channel missing/.test(r.stderr));
  check('invalid index names the missing artifact fields', /sha256 missing/.test(r.stderr) && /size missing/.test(r.stderr));
}

// A missing file argument is a usage error, not a crash or a false pass.
{
  const r = spawnSync(process.execPath, [VALIDATOR], { encoding: 'utf8' });
  check('missing argument exits non-zero', r.status !== 0);
  check('missing argument prints usage', /Usage:/.test(r.stderr));
}

if (failures > 0) {
  console.error(`\n${failures} check(s) failed`);
  process.exit(1);
}
console.log('\nAll validate-firmware-index checks passed');
