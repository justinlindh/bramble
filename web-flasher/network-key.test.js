import { test } from 'node:test';
import assert from 'node:assert/strict';
import { parseNetworkKeyInput, networkKeyFingerprint } from './network-key.js';

const KEY = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef';

test('parses a bare 64-hex key', () => {
    assert.equal(parseNetworkKeyInput(KEY), KEY);
});

test('normalizes case and surrounding whitespace', () => {
    assert.equal(parseNetworkKeyInput(`  ${KEY.toUpperCase()}\n`), KEY);
});

test('parses a share string', () => {
    assert.equal(parseNetworkKeyInput(`bramble://net/v1?k=${KEY}`), KEY);
});

test('parses a share string with extra params in any order', () => {
    assert.equal(parseNetworkKeyInput(`bramble://net/v1?n=Field&k=${KEY}`), KEY);
});

test('rejects a share string with no key', () => {
    assert.throws(() => parseNetworkKeyInput('bramble://net/v1?n=Field'), /no network key/);
});

test('rejects a short key', () => {
    assert.throws(() => parseNetworkKeyInput('abc'), /64 hex characters/);
});

test('rejects a non-hex key of the right length', () => {
    assert.throws(() => parseNetworkKeyInput('z'.repeat(64)), /64 hex characters/);
});

test('rejects an anchor share string, which is a different secret', () => {
    assert.throws(() => parseNetworkKeyInput(`bramble://anchor/v1?sk=${KEY}`), /64 hex characters/);
});

test('rejects blank input with a skip hint', () => {
    assert.throws(() => parseNetworkKeyInput(''), /leave the field blank to skip/);
    assert.throws(() => parseNetworkKeyInput('   '), /leave the field blank to skip/);
});

test('fingerprint matches the known answer for the all-zero key', async () => {
    // SHA256 of 32 zero bytes starts 66687aad, so the fingerprint is 66687aad.
    assert.equal(await networkKeyFingerprint('0'.repeat(64)), '66687aad');
});

test('fingerprint is 8 lowercase hex and stable for a given key', async () => {
    const a = await networkKeyFingerprint(KEY);
    const b = await networkKeyFingerprint(KEY);
    assert.match(a, /^[0-9a-f]{8}$/);
    assert.equal(a, b);
});

test('different keys yield different fingerprints', async () => {
    const a = await networkKeyFingerprint(KEY);
    const b = await networkKeyFingerprint('f'.repeat(64));
    assert.notEqual(a, b);
});
