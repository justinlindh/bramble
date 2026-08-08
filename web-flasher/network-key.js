/**
 * Network-key handling for the flasher's Device Setup step.
 *
 * A freshly flashed node has no network key and is INERT: it neither emits nor
 * accepts authenticated control-plane traffic, so it does not mesh at all until
 * a key is provisioned. The flasher offers the JOIN half of that: paste the key
 * from a node that already has one. Founding a new network (minting a key) is
 * deliberately left to the web app, which has the QR, the copy-confirm, the
 * persistent fingerprint readout and the re-key guard that a one-shot page you
 * close cannot offer.
 *
 * Mirrors webapp/src/utils/networkKeyShare.ts for the share-string format and
 * the firmware's network_key_fingerprint for the fingerprint derivation.
 */

const SHARE_PREFIX = 'bramble://net/v1?';
const HEX64 = /^[0-9a-fA-F]{64}$/;

/**
 * Accepts a `bramble://net/v1?k=<hex>` share string or a bare 64-hex key and
 * returns the normalized lowercase hex key. Throws on anything else.
 */
export function parseNetworkKeyInput(input) {
    const raw = String(input ?? '').trim();
    if (!raw) {
        throw new Error('Enter a network key, or leave the field blank to skip.');
    }

    let candidate = raw;
    if (raw.startsWith(SHARE_PREFIX)) {
        const params = new URLSearchParams(raw.slice(SHARE_PREFIX.length));
        const k = params.get('k');
        if (!k) {
            throw new Error('That share string has no network key in it (expected "k=").');
        }
        candidate = k.trim();
    }

    if (!HEX64.test(candidate)) {
        throw new Error('A network key is 64 hex characters, or a bramble://net/v1?k=... string.');
    }
    return candidate.toLowerCase();
}

/**
 * SHA256(key)[0:4] as 8 lowercase hex, matching what every node reports from
 * bramble.getNetworkKeyStatus. Comparing this against the founder node is how
 * an operator confirms the fleet converged without moving the secret again.
 */
export async function networkKeyFingerprint(keyHex) {
    const bytes = new Uint8Array(32);
    for (let i = 0; i < 32; i++) {
        bytes[i] = parseInt(keyHex.slice(i * 2, i * 2 + 2), 16);
    }
    const digest = await globalThis.crypto.subtle.digest('SHA-256', bytes);
    return Array.from(new Uint8Array(digest).slice(0, 4))
        .map((b) => b.toString(16).padStart(2, '0'))
        .join('');
}
