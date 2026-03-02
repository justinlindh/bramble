import test from 'node:test';
import assert from 'node:assert/strict';

import { buildWifiConfigCommands } from './wifi-config.js';

test('buildWifiConfigCommands builds wifi/reboot command sequence', () => {
  const commands = buildWifiConfigCommands({
    ssid: 'MyNetwork',
    password: 'supersecret',
    rebootAfter: true,
  });

  assert.deepEqual(commands, [
    'wifi set MyNetwork supersecret',
    'reboot',
  ]);
});

test('buildWifiConfigCommands allows open networks', () => {
  const commands = buildWifiConfigCommands({
    ssid: 'OpenNetwork',
    password: '',
    rebootAfter: false,
  });

  assert.deepEqual(commands, ['wifi set OpenNetwork']);
});

test('buildWifiConfigCommands rejects ssid with spaces', () => {
  assert.throws(
    () => buildWifiConfigCommands({ ssid: 'My Wifi', password: 'pw' }),
    /cannot contain spaces/i,
  );
});
