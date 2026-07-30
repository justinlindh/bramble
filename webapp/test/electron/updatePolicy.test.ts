import { describe, it, expect } from 'vitest';
import {
  compareSemver,
  decideUpdate,
  feedBaseUrl,
  parseWebappTag,
  pickLatestWebappRelease,
  releaseHtmlUrl,
  updateCapability,
  type GithubRelease,
} from '../../electron/updatePolicy';

describe('compareSemver', () => {
  it('orders by core version', () => {
    expect(compareSemver('1.4.0', '1.5.0')).toBeLessThan(0);
    expect(compareSemver('2.0.0', '1.9.9')).toBeGreaterThan(0);
    expect(compareSemver('1.4.0', '1.4.0')).toBe(0);
  });

  it('treats a prerelease as older than its release', () => {
    expect(compareSemver('1.5.0-rc.1', '1.5.0')).toBeLessThan(0);
    expect(compareSemver('1.5.0', '1.5.0-rc.1')).toBeGreaterThan(0);
    expect(compareSemver('1.5.0-rc.1', '1.5.0-rc.2')).toBeLessThan(0);
  });

  it('tolerates a leading v', () => {
    expect(compareSemver('v1.4.0', '1.5.0')).toBeLessThan(0);
  });
});

describe('parseWebappTag', () => {
  it('accepts webapp tags and returns the core version', () => {
    expect(parseWebappTag('webapp-v1.5.0')).toBe('1.5.0');
    expect(parseWebappTag('webapp-v2.0.0-rc.1')).toBe('2.0.0-rc.1');
    expect(parseWebappTag('  webapp-v1.5.0  ')).toBe('1.5.0');
  });

  it('rejects other components and junk', () => {
    expect(parseWebappTag('firmware-v1.5.0')).toBeNull();
    expect(parseWebappTag('protocol-v1.0.0')).toBeNull();
    expect(parseWebappTag('sim-v1.0.0')).toBeNull();
    expect(parseWebappTag('v1.5.0')).toBeNull();
    expect(parseWebappTag('webapp-1.5.0')).toBeNull();
    expect(parseWebappTag('')).toBeNull();
  });
});

describe('pickLatestWebappRelease', () => {
  const rel = (tag: string, extra: Partial<GithubRelease> = {}): GithubRelease => ({
    tag_name: tag,
    ...extra,
  });

  it('ignores other components and picks the highest webapp version', () => {
    const releases = [
      rel('firmware-v9.9.9'),
      rel('webapp-v1.4.0'),
      rel('protocol-v3.0.0'),
      rel('webapp-v1.5.0'),
      rel('sim-v2.0.0'),
      rel('webapp-v1.4.9'),
    ];
    const latest = pickLatestWebappRelease(releases);
    expect(latest?.tag).toBe('webapp-v1.5.0');
    expect(latest?.version).toBe('1.5.0');
  });

  it('skips drafts and prereleases', () => {
    const releases = [
      rel('webapp-v2.0.0', { draft: true }),
      rel('webapp-v1.9.0', { prerelease: true }),
      rel('webapp-v1.5.0'),
    ];
    expect(pickLatestWebappRelease(releases)?.version).toBe('1.5.0');
  });

  it('returns null when there is no webapp release', () => {
    expect(pickLatestWebappRelease([rel('firmware-v1.0.0')])).toBeNull();
    expect(pickLatestWebappRelease([])).toBeNull();
  });

  it('falls back to a constructed release page URL when html_url is absent', () => {
    const latest = pickLatestWebappRelease([{ tag_name: 'webapp-v1.5.0' }]);
    expect(latest?.htmlUrl).toBe(releaseHtmlUrl('webapp-v1.5.0'));
  });
});

describe('updateCapability', () => {
  it('is in-app on Windows', () => {
    expect(updateCapability('win32', {})).toBe('in-app');
  });

  it('is in-app on Linux only when running from an AppImage', () => {
    expect(updateCapability('linux', { APPIMAGE: '/tmp/Bramble.AppImage' })).toBe('in-app');
    expect(updateCapability('linux', {})).toBe('manual');
  });

  it('is manual on macOS (unsigned dmg)', () => {
    expect(updateCapability('darwin', {})).toBe('manual');
  });
});

describe('feedBaseUrl', () => {
  it('points at the per-tag release asset directory', () => {
    expect(feedBaseUrl('webapp-v1.5.0')).toBe(
      'https://github.com/justinlindh/bramble/releases/download/webapp-v1.5.0/',
    );
  });
});

describe('decideUpdate', () => {
  const latest = { tag: 'webapp-v1.5.0', version: '1.5.0', htmlUrl: 'https://example.test/rel' };

  it('is up-to-date when nothing is newer', () => {
    expect(decideUpdate({ currentVersion: '1.5.0', latest, capability: 'in-app' })).toEqual({
      kind: 'up-to-date',
    });
    expect(decideUpdate({ currentVersion: '1.6.0', latest, capability: 'in-app' })).toEqual({
      kind: 'up-to-date',
    });
    expect(decideUpdate({ currentVersion: '1.4.0', latest: null, capability: 'in-app' })).toEqual({
      kind: 'up-to-date',
    });
  });

  it('routes a newer version to the in-app path when capable', () => {
    const decision = decideUpdate({ currentVersion: '1.4.0', latest, capability: 'in-app' });
    expect(decision).toEqual({
      kind: 'in-app',
      version: '1.5.0',
      tag: 'webapp-v1.5.0',
      feedUrl: 'https://github.com/justinlindh/bramble/releases/download/webapp-v1.5.0/',
    });
  });

  it('routes a newer version to the manual path when not capable', () => {
    const decision = decideUpdate({ currentVersion: '1.4.0', latest, capability: 'manual' });
    expect(decision).toEqual({
      kind: 'manual',
      version: '1.5.0',
      releaseUrl: 'https://example.test/rel',
    });
  });
});
