// Semver precedence comparison (https://semver.org/#spec-item-11), shared by
// the OTA firmware update flow (src/lib/otaIndex.ts) and the desktop
// self-updater (electron/updatePolicy.ts) so the two agree on version ordering.
// A leading 'v' and surrounding whitespace are tolerated on either input.

function parseCore(v: string): { core: number[]; pre: string } {
  const s = v.trim().replace(/^v/, '');
  const dash = s.indexOf('-');
  const core = (dash >= 0 ? s.slice(0, dash) : s).split('.').map((n) => Number(n) || 0);
  while (core.length < 3) core.push(0);
  return { core: core.slice(0, 3), pre: dash >= 0 ? s.slice(dash + 1) : '' };
}

// Semver precedence for a single dot-separated prerelease identifier: purely-
// numeric identifiers compare numerically and always rank below alphanumeric
// identifiers, which compare lexically (ASCII order).
function compareIdentifier(a: string, b: string): number {
  const aNum = /^[0-9]+$/.test(a);
  const bNum = /^[0-9]+$/.test(b);
  if (aNum && bNum) return Number(a) - Number(b);
  if (aNum) return -1;
  if (bNum) return 1;
  return a < b ? -1 : a > b ? 1 : 0;
}

function comparePrerelease(a: string, b: string): number {
  if (a === b) return 0;
  if (a === '') return 1;   // release above prerelease
  if (b === '') return -1;
  const aParts = a.split('.');
  const bParts = b.split('.');
  const len = Math.min(aParts.length, bParts.length);
  for (let i = 0; i < len; i++) {
    const c = compareIdentifier(aParts[i], bParts[i]);
    if (c !== 0) return c;
  }
  return aParts.length - bParts.length; // shorter prerelease ranks lower
}

// Semver-aware compare: negative if a < b, positive if a > b, 0 if equal.
export function compareSemver(a: string, b: string): number {
  const pa = parseCore(a);
  const pb = parseCore(b);
  for (let i = 0; i < 3; i++) {
    if (pa.core[i] !== pb.core[i]) return pa.core[i] - pb.core[i];
  }
  return comparePrerelease(pa.pre, pb.pre);
}
