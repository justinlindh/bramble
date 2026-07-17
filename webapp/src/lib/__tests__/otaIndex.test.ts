import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import {
  appArtifactForBoard, compareVersions, fetchOtaIndex, findUpdate, hardwareToBoard,
  relativizeArtifactPath, releasesForBoard, type OtaRelease,
} from '../otaIndex';

const releases: OtaRelease[] = [
  { version: 'v1.4.0', publishedAt: '2026-07-01T00:00:00Z', artifacts: [
    { board: 'heltec-v4', file: '/ota/stable/v1.4.0/heltec-v4/bramble.bin' },
    // Real generator shape (scripts/build-firmware-index.js): every file in
    // the board directory gets an artifact entry, not just the app image.
    { board: 'heltec-v3', file: '/ota/stable/v1.4.0/heltec-v3/bootloader.bin' },
    { board: 'heltec-v3', file: '/ota/stable/v1.4.0/heltec-v3/partition-table.bin' },
    { board: 'heltec-v3', file: '/ota/stable/v1.4.0/heltec-v3/bramble.bin' },
    { board: 'heltec-v3', file: '/ota/stable/v1.4.0/heltec-v3/bramble-1.4.0.bin' },
  ]},
  { version: 'v1.3.9', publishedAt: '2026-06-01T00:00:00Z', artifacts: [
    { board: 'heltec-v4', file: '/ota/stable/v1.3.9/heltec-v4/bramble.bin' },
  ]},
  { version: 'v2.0.0', publishedAt: '2026-07-10T00:00:00Z', artifacts: [
    { board: 'tdeck-plus', file: '/ota/stable/v2.0.0/tdeck-plus/bramble.bin' },
  ]},
  { version: 'v1.2.0', publishedAt: '2026-05-01T00:00:00Z', artifacts: [
    // Bootloader/partition only for this board: NOT installable for heltec-v3.
    { board: 'heltec-v3', file: '/ota/stable/v1.2.0/heltec-v3/bootloader.bin' },
    { board: 'heltec-v3', file: '/ota/stable/v1.2.0/heltec-v3/partition-table.bin' },
  ]},
];

describe('hardwareToBoard', () => {
  it('maps firmware underscores to index hyphens', () => {
    expect(hardwareToBoard('heltec_v4')).toBe('heltec-v4');
    expect(hardwareToBoard('tdeck_plus')).toBe('tdeck-plus');
  });
});

describe('compareVersions', () => {
  it('orders numerically, not lexically', () => {
    expect(compareVersions('v1.10.0', 'v1.9.0')).toBeGreaterThan(0);
  });
  it('treats prerelease as older than the release core', () => {
    expect(compareVersions('1.4.0-rc1', '1.4.0')).toBeLessThan(0);
  });
  it('handles the dev version string', () => {
    expect(compareVersions('v1.4.0', '0.0.0-local')).toBeGreaterThan(0);
  });
  it('compares numeric prerelease identifiers numerically, not lexically', () => {
    expect(compareVersions('1.4.0-rc.10', '1.4.0-rc.9')).toBeGreaterThan(0);
    expect(compareVersions('1.4.0-rc.2', '1.4.0-rc.10')).toBeLessThan(0);
  });
  it('ranks a shorter prerelease lower when the shared identifiers are equal', () => {
    expect(compareVersions('1.4.0-alpha', '1.4.0-alpha.1')).toBeLessThan(0);
  });
  it('ranks numeric identifiers below alphanumeric identifiers', () => {
    expect(compareVersions('1.4.0-1', '1.4.0-alpha')).toBeLessThan(0);
  });
});

describe('appArtifactForBoard', () => {
  const release = releases[0]; // v1.4.0: heltec-v3 has bootloader/partition/canonical/tagged

  it('picks the canonical bramble.bin when present', () => {
    const artifact = appArtifactForBoard(release, 'heltec-v3');
    expect(artifact?.file).toBe('/ota/stable/v1.4.0/heltec-v3/bramble.bin');
  });

  it('falls back to the semver-tagged copy when there is no canonical file', () => {
    const taggedOnly: OtaRelease = {
      version: 'v1.5.0', publishedAt: '2026-07-15T00:00:00Z', artifacts: [
        { board: 'heltec-v3', file: '/ota/stable/v1.5.0/heltec-v3/bootloader.bin' },
        { board: 'heltec-v3', file: '/ota/stable/v1.5.0/heltec-v3/partition-table.bin' },
        { board: 'heltec-v3', file: '/ota/stable/v1.5.0/heltec-v3/bramble-1.5.0.bin' },
      ],
    };
    const artifact = appArtifactForBoard(taggedOnly, 'heltec-v3');
    expect(artifact?.file).toBe('/ota/stable/v1.5.0/heltec-v3/bramble-1.5.0.bin');
  });

  it('returns null when only bootloader/partition files exist for the board', () => {
    const artifact = appArtifactForBoard(releases[3], 'heltec-v3'); // v1.2.0
    expect(artifact).toBeNull();
  });

  it('returns null when the board has no artifacts at all', () => {
    expect(appArtifactForBoard(release, 'tdeck-plus')).toBeNull();
  });
});

describe('relativizeArtifactPath', () => {
  it('strips the origin path prefix', () => {
    expect(relativizeArtifactPath('/ota/stable/v1.4.0/heltec-v4/bramble.bin',
      'https://bramblemesh.org/ota/')).toBe('stable/v1.4.0/heltec-v4/bramble.bin');
  });
  it('passes through already-relative files', () => {
    expect(relativizeArtifactPath('bramble.bin', 'http://192.0.2.199:8088/')).toBe('bramble.bin');
  });
  it('returns null when the file is outside the origin', () => {
    expect(relativizeArtifactPath('/elsewhere/bramble.bin', 'https://bramblemesh.org/ota/')).toBeNull();
  });
  it('strips the origin path prefix when the origin has no trailing slash', () => {
    expect(relativizeArtifactPath('/firmware/stable/v1.4.0/heltec-v4/bramble.bin',
      'https://ota.example/firmware')).toBe('stable/v1.4.0/heltec-v4/bramble.bin');
  });
});

describe('releasesForBoard / findUpdate', () => {
  it('filters to the board and sorts newest first', () => {
    const r = releasesForBoard(releases, 'heltec-v4');
    expect(r.map((x) => x.version)).toEqual(['v1.4.0', 'v1.3.9']);
  });
  it('finds the newest strictly-newer release', () => {
    expect(findUpdate(releases, 'heltec-v4', 'v1.3.9')?.version).toBe('v1.4.0');
    expect(findUpdate(releases, 'heltec-v4', 'v1.4.0')).toBeNull();
  });
  it('excludes a release whose only board artifacts are bootloader/partition files', () => {
    const r = releasesForBoard(releases, 'heltec-v3');
    expect(r.map((x) => x.version)).toEqual(['v1.4.0']);
    expect(r.map((x) => x.version)).not.toContain('v1.2.0');
  });
});

describe('fetchOtaIndex', () => {
  const origin = 'https://bramblemesh.org/ota/';
  let fetchMock: ReturnType<typeof vi.fn>;

  function jsonResponse(body: unknown, ok = true): Response {
    return { ok, json: () => Promise.resolve(body) } as unknown as Response;
  }

  beforeEach(() => {
    fetchMock = vi.fn();
    vi.stubGlobal('fetch', fetchMock);
  });

  afterEach(() => {
    vi.unstubAllGlobals();
  });

  it('normalizes a well-formed index and GETs <origin>/index.json', async () => {
    fetchMock.mockResolvedValue(jsonResponse({
      releases: [
        {
          version: 'v1.4.0',
          published_at: '2026-07-01T00:00:00Z',
          channel: 'stable',
          artifacts: [
            {
              board: 'heltec-v4',
              file: '/ota/stable/v1.4.0/heltec-v4/bramble.bin',
              sha256: 'abc123',
              size: 123456,
              notes: 'first cut',
            },
          ],
        },
      ],
    }));

    const result = await fetchOtaIndex(origin);

    expect(fetchMock).toHaveBeenCalledWith('https://bramblemesh.org/ota/index.json');
    expect(result).toEqual([
      {
        version: 'v1.4.0',
        publishedAt: '2026-07-01T00:00:00Z',
        channel: 'stable',
        artifacts: [
          {
            board: 'heltec-v4',
            file: '/ota/stable/v1.4.0/heltec-v4/bramble.bin',
            sha256: 'abc123',
            size: 123456,
            notes: 'first cut',
          },
        ],
      },
    ]);
  });

  it('tolerates a release with only the required fields', async () => {
    fetchMock.mockResolvedValue(jsonResponse({
      releases: [
        { version: 'v1.4.0', artifacts: [{ board: 'heltec-v4', file: 'bramble.bin' }] },
      ],
    }));

    const result = await fetchOtaIndex(origin);

    expect(result).toEqual([
      {
        version: 'v1.4.0',
        publishedAt: '',
        channel: undefined,
        artifacts: [
          { board: 'heltec-v4', file: 'bramble.bin', sha256: undefined, size: undefined, notes: undefined },
        ],
      },
    ]);
  });

  it('drops entries with no version or no artifacts', async () => {
    fetchMock.mockResolvedValue(jsonResponse({
      releases: [
        { version: '', artifacts: [{ board: 'heltec-v4', file: 'bramble.bin' }] },
        { version: 'v1.5.0', artifacts: [] },
        { artifacts: [{ board: 'heltec-v4', file: 'bramble.bin' }] },
        { version: 'v1.4.0', artifacts: [{ board: 'heltec-v4', file: 'bramble.bin' }] },
      ],
    }));

    const result = await fetchOtaIndex(origin);

    expect(result.map((r) => r.version)).toEqual(['v1.4.0']);
  });

  it('returns an empty array when releases is missing or not an array', async () => {
    fetchMock.mockResolvedValueOnce(jsonResponse({}));
    expect(await fetchOtaIndex(origin)).toEqual([]);

    fetchMock.mockResolvedValueOnce(jsonResponse({ releases: 'nope' }));
    expect(await fetchOtaIndex(origin)).toEqual([]);
  });

  it('throws a user-displayable error when the network request rejects', async () => {
    fetchMock.mockRejectedValue(new Error('network down'));

    await expect(fetchOtaIndex(origin)).rejects.toThrow(
      'Could not load the release index from https://bramblemesh.org/ota/index.json',
    );
  });

  it('throws a user-displayable error on a non-2xx response', async () => {
    fetchMock.mockResolvedValue(jsonResponse({}, false));

    await expect(fetchOtaIndex(origin)).rejects.toThrow(
      'Could not load the release index from https://bramblemesh.org/ota/index.json',
    );
  });

  it('throws a user-displayable error when the response body is not valid JSON', async () => {
    fetchMock.mockResolvedValue({
      ok: true,
      json: () => Promise.reject(new Error('bad json')),
    } as unknown as Response);

    await expect(fetchOtaIndex(origin)).rejects.toThrow(
      'Could not load the release index from https://bramblemesh.org/ota/index.json',
    );
  });

  it('GETs <origin>/index.json when the origin has no trailing slash', async () => {
    fetchMock.mockResolvedValue(jsonResponse({ releases: [] }));

    await fetchOtaIndex('https://ota.example/firmware');

    expect(fetchMock).toHaveBeenCalledWith('https://ota.example/firmware/index.json');
  });

  it('throws a user-displayable error, not a raw TypeError, for a malformed origin', async () => {
    let caught: unknown;
    try {
      await fetchOtaIndex('not a url');
      throw new Error('expected fetchOtaIndex to throw');
    } catch (e) {
      caught = e;
    }

    expect(caught).toBeInstanceOf(Error);
    expect(caught).not.toBeInstanceOf(TypeError);
    expect((caught as Error).message).toBe('Could not load the release index from not a url');
    expect(fetchMock).not.toHaveBeenCalled();
  });
});

describe('fetchOtaIndex (Electron path)', () => {
  const origin = 'https://bramblemesh.org/ota/';
  let fetchMock: ReturnType<typeof vi.fn>;
  let desktopFetchMock: ReturnType<typeof vi.fn>;

  beforeEach(() => {
    fetchMock = vi.fn();
    vi.stubGlobal('fetch', fetchMock);
    vi.stubGlobal('isElectron', true);
    desktopFetchMock = vi.fn();
    window.brambleDesktop = {
      startDiscovery: vi.fn(),
      stopDiscovery: vi.fn(),
      onDiscovered: vi.fn(() => () => {}),
      onDevicePicker: vi.fn(() => () => {}),
      selectDevice: vi.fn(),
      cancelDevicePicker: vi.fn(),
      autoSelectNextDevice: vi.fn(),
      fetchOtaIndex: desktopFetchMock,
    };
  });

  afterEach(() => {
    vi.unstubAllGlobals();
    delete window.brambleDesktop;
  });

  it('fetches through the desktop bridge instead of global fetch, and normalizes the result', async () => {
    desktopFetchMock.mockResolvedValue({
      ok: true,
      status: 200,
      body: JSON.stringify({
        releases: [
          { version: 'v1.4.0', published_at: '2026-07-01T00:00:00Z', artifacts: [
            { board: 'heltec-v4', file: 'bramble.bin' },
          ] },
        ],
      }),
    });

    const result = await fetchOtaIndex(origin);

    expect(desktopFetchMock).toHaveBeenCalledWith('https://bramblemesh.org/ota/index.json');
    expect(fetchMock).not.toHaveBeenCalled();
    expect(result).toEqual([
      {
        version: 'v1.4.0',
        publishedAt: '2026-07-01T00:00:00Z',
        channel: undefined,
        artifacts: [
          { board: 'heltec-v4', file: 'bramble.bin', sha256: undefined, size: undefined, notes: undefined },
        ],
      },
    ]);
  });

  it('throws a user-displayable error on a non-2xx status from the bridge', async () => {
    desktopFetchMock.mockResolvedValue({ ok: false, status: 404, body: '' });

    await expect(fetchOtaIndex(origin)).rejects.toThrow(
      'Could not load the release index from https://bramblemesh.org/ota/index.json',
    );
    expect(fetchMock).not.toHaveBeenCalled();
  });

  it('throws a user-displayable error when the bridge body is not valid JSON', async () => {
    desktopFetchMock.mockResolvedValue({ ok: true, status: 200, body: 'not json' });

    await expect(fetchOtaIndex(origin)).rejects.toThrow(
      'Could not load the release index from https://bramblemesh.org/ota/index.json',
    );
    expect(fetchMock).not.toHaveBeenCalled();
  });
});
