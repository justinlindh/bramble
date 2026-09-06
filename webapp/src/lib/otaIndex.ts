// Release-index consumption for the OTA journey (docs/ota-release-schema.md).
// The index lives at <origin>/index.json; artifact `file` fields are absolute
// paths under the origin, but bramble.otaUpdate wants origin-relative paths.

import { isElectron } from '../utils/platform';
import { compareSemver } from './semver';

export interface OtaArtifact { board: string; file: string; sha256?: string; size?: number; notes?: string }
export interface OtaRelease { version: string; publishedAt: string; channel?: string; artifacts: OtaArtifact[] }

export function hardwareToBoard(hardware: string): string {
  return hardware.replaceAll('_', '-');
}

export function relativizeArtifactPath(file: string, origin: string): string | null {
  if (!file.startsWith('/')) return file;
  let originPath: string;
  try {
    originPath = new URL(origin).pathname;
  } catch {
    return null;
  }
  if (!originPath.endsWith('/')) originPath += '/';
  return file.startsWith(originPath) ? file.slice(originPath.length) : null;
}

// The generator (scripts/build-firmware-index.js) writes an artifact entry
// for every file in a board's release directory, per docs/ota-release-schema.md
// "Filename policy": bootloader.bin, partition-table.bin, the canonical app
// image bramble.bin, and a semver-tagged copy bramble-<version>.bin. The
// serial web-flasher wants the bootloader/partition entries too, so the
// generator must keep emitting all of them; the webapp OTA journey only
// ever installs the app image, so it must pick that one out.
const TAGGED_APP_FILE = /^bramble-.+\.bin$/;

function basename(file: string): string {
  const slash = file.lastIndexOf('/');
  return slash >= 0 ? file.slice(slash + 1) : file;
}

export function appArtifactForBoard(release: OtaRelease, board: string): OtaArtifact | null {
  const forBoard = release.artifacts.filter((a) => a.board === board);
  const canonical = forBoard.find((a) => basename(a.file) === 'bramble.bin');
  if (canonical) return canonical;
  const tagged = forBoard.find((a) => TAGGED_APP_FILE.test(basename(a.file)));
  return tagged ?? null;
}

export function releasesForBoard(releases: OtaRelease[], board: string): OtaRelease[] {
  return releases
    .filter((r) => appArtifactForBoard(r, board) !== null)
    .sort((x, y) => {
      const byVersion = compareSemver(y.version, x.version);
      if (byVersion !== 0) return byVersion;
      return (Date.parse(y.publishedAt) || 0) - (Date.parse(x.publishedAt) || 0);
    });
}

export function findUpdate(releases: OtaRelease[], board: string, running: string): OtaRelease | null {
  const candidates = releasesForBoard(releases, board);
  const newest = candidates[0];
  return newest && compareSemver(newest.version, running) > 0 ? newest : null;
}

interface RawOtaArtifact {
  board?: unknown;
  file?: unknown;
  sha256?: unknown;
  size?: unknown;
  notes?: unknown;
}

interface RawOtaRelease {
  version?: unknown;
  published_at?: unknown;
  channel?: unknown;
  artifacts?: unknown;
}

interface RawOtaIndex {
  releases?: unknown;
}

function normalizeIndexData(data: unknown): OtaRelease[] {
  const rawReleases = (data as RawOtaIndex | null)?.releases;
  const raw: RawOtaRelease[] = Array.isArray(rawReleases) ? (rawReleases as RawOtaRelease[]) : [];
  return raw
    .map((r): OtaRelease => {
      const rawArtifacts = Array.isArray(r.artifacts) ? (r.artifacts as RawOtaArtifact[]) : [];
      return {
        version: String(r.version ?? ''),
        publishedAt: String(r.published_at ?? ''),
        channel: r.channel ? String(r.channel) : undefined,
        artifacts: rawArtifacts.map((a) => ({
          board: String(a.board ?? ''),
          file: String(a.file ?? ''),
          sha256: a.sha256 ? String(a.sha256) : undefined,
          size: typeof a.size === 'number' ? a.size : undefined,
          notes: a.notes ? String(a.notes) : undefined,
        })),
      };
    })
    .filter((r) => r.version && r.artifacts.length > 0);
}

/**
 * Fetches the index body and returns it parsed as JSON, or throws a
 * user-displayable error on any transport/status/parse problem. Picks one
 * body-getter for the environment up front (the Electron main-process bridge,
 * which sidesteps renderer CORS on packaged desktop builds, or plain
 * fetch()), then runs both through the same check-ok/parse/throw path.
 */
async function loadIndexData(url: string): Promise<unknown> {
  const bridge = isElectron() ? window.brambleDesktop?.fetchOtaIndex : undefined;
  const getBody: (u: string) => Promise<{ ok: boolean; body: unknown }> = bridge
    ? async (u) => {
        const r = await bridge(u);
        return { ok: r.ok, body: r.ok ? JSON.parse(r.body) : undefined };
      }
    : async (u) => {
        const resp = await fetch(u);
        return { ok: resp.ok, body: resp.ok ? await resp.json() : undefined };
      };

  const fail = () => new Error(`Could not load the release index from ${url}`);
  let result: { ok: boolean; body: unknown };
  try {
    result = await getBody(url);
  } catch {
    throw fail();
  }
  if (!result.ok) throw fail();
  return result.body;
}

export async function fetchOtaIndex(origin: string): Promise<OtaRelease[]> {
  let url: string;
  try {
    // new URL('index.json', base) resolves relative to base's last path
    // segment, so a no-trailing-slash origin ('https://x/firmware') would
    // drop 'firmware' and resolve to 'https://x/index.json'. The firmware
    // accepts no-trailing-slash origins and joins with '/' itself, so
    // normalize here before resolving against it.
    const base = origin.endsWith('/') ? origin : `${origin}/`;
    url = new URL('index.json', base).toString();
  } catch {
    throw new Error(`Could not load the release index from ${origin}`);
  }
  const data = await loadIndexData(url);
  return normalizeIndexData(data);
}
