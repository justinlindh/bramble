import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';

const root = new URL('.', import.meta.url).pathname;

function read(name) {
  return readFileSync(join(root, name), 'utf8');
}

test('index.html loads flasher as module and defines esptool-js importmap', () => {
  const html = read('index.html');
  assert.match(html, /<script\s+type="importmap">[\s\S]*"esptool-js"\s*:\s*"https:\/\/unpkg\.com\/esptool-js@0\.5\.7\/bundle\.js"[\s\S]*<\/script>/);
  assert.match(html, /<script\s+type="module"\s+src="flasher\.js"><\/script>/);
  assert.doesNotMatch(html, /<script\s+src="https:\/\/unpkg\.com\/esptool-js@0\.5\.7\/bundle\.js"><\/script>/);
});

test('every board erases otadata at 0xe000 so a reflashed device boots the new app in ota_0', () => {
  const js = read('flasher.js');
  const boardsSrc = js.slice(js.indexOf('const BOARDS'), js.indexOf('(function ()'));
  const partitionLists = boardsSrc.match(/partitions:\s*\[[\s\S]*?\]/g) || [];
  assert.equal(partitionLists.length, 3, 'expected partition lists for all three boards');
  for (const list of partitionLists) {
    assert.match(list, /offset:\s*0xe000,\s*blank:\s*0x2000/);
  }
});

test('flasher.js imports ESPLoader and Transport directly and does not use global esptool', () => {
  const js = read('flasher.js');
  assert.match(js, /^import\s+\{\s*ESPLoader\s*,\s*Transport\s*\}\s+from\s+['"]esptool-js['"];/m);
  assert.match(js, /new\s+Transport\(/);
  assert.match(js, /new\s+ESPLoader\(/);
  assert.doesNotMatch(js, /\besptool\./);
});

test('user-derived text never reaches an HTML-injection sink', () => {
  const js = read('flasher.js');
  // The done-screen message interpolates user-typed values (device name,
  // SSID, auth token), so it must be rendered via text nodes. The only
  // permitted innerHTML use is clearing with a constant empty string.
  const uses = js.match(/innerHTML\s*=\s*[^;]+/g) || [];
  for (const use of uses) {
    assert.match(use, /^innerHTML\s*=\s*''$/, `non-constant innerHTML assignment: ${use}`);
  }
  assert.doesNotMatch(js, /insertAdjacentHTML|outerHTML\s*=|document\.write/);
});
