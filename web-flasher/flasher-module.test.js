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

test('flasher.js imports ESPLoader and Transport directly and does not use global esptool', () => {
  const js = read('flasher.js');
  assert.match(js, /^import\s+\{\s*ESPLoader\s*,\s*Transport\s*\}\s+from\s+['"]esptool-js['"];/m);
  assert.match(js, /new\s+Transport\(/);
  assert.match(js, /new\s+ESPLoader\(/);
  assert.doesNotMatch(js, /\besptool\./);
});
