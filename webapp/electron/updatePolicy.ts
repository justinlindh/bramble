// Pure decision logic for the desktop auto-updater. Deliberately imports
// nothing from electron or electron-updater so it can be unit tested directly
// (see test/electron/updatePolicy.test.ts). All IO (GitHub fetch, dialogs,
// electron-updater wiring) lives in electron/updater.ts, which consumes these
// helpers.

/** Owner/repo of the public GitHub repository releases are cut against. */
export const GITHUB_OWNER = 'justinlindh';
export const GITHUB_REPO = 'bramble';

/** A single release entry as returned by the GitHub REST releases API. */
export interface GithubRelease {
  tag_name: string;
  draft?: boolean;
  prerelease?: boolean;
  html_url?: string;
}

/** The webapp release chosen as the update candidate. */
export interface WebappRelease {
  /** Full git tag, e.g. "webapp-v1.5.0". */
  tag: string;
  /** Semver core parsed out of the tag, e.g. "1.5.0". */
  version: string;
  /** GitHub release page, used for the manual (open-in-browser) path. */
  htmlUrl: string;
}

/**
 * How this install can take an update:
 *   'in-app'  the running format supports electron-updater download+install
 *             (Windows NSIS, Linux AppImage).
 *   'manual'  the format cannot self-update (Linux deb/pacman are owned by the
 *             system package manager; macOS builds are unsigned, so Squirrel.Mac
 *             refuses to apply them). We notify and open the releases page.
 */
export type UpdateCapability = 'in-app' | 'manual';

/** The action the updater glue should take after a check. */
export type UpdateDecision =
  | { kind: 'up-to-date' }
  | { kind: 'in-app'; version: string; tag: string; feedUrl: string }
  | { kind: 'manual'; version: string; releaseUrl: string };

const WEBAPP_TAG_RE = /^webapp-v(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?$/;

interface ParsedVersion {
  core: [number, number, number];
  pre: string[];
}

function parseCore(version: string): ParsedVersion {
  const clean = version.trim().replace(/^v/, '');
  const [corePart, prePart] = clean.split('-', 2);
  const core = corePart.split('.').map((n) => Number.parseInt(n, 10));
  return {
    core: [core[0] || 0, core[1] || 0, core[2] || 0],
    pre: prePart ? prePart.split('.') : [],
  };
}

function comparePrerelease(a: string[], b: string[]): number {
  // A version with no prerelease outranks one that has a prerelease (1.0.0 >
  // 1.0.0-rc.1), matching semver precedence.
  if (a.length === 0 && b.length === 0) return 0;
  if (a.length === 0) return 1;
  if (b.length === 0) return -1;
  const len = Math.max(a.length, b.length);
  for (let i = 0; i < len; i++) {
    if (i >= a.length) return -1;
    if (i >= b.length) return 1;
    const ai = a[i];
    const bi = b[i];
    const an = /^\d+$/.test(ai);
    const bn = /^\d+$/.test(bi);
    if (an && bn) {
      const d = Number.parseInt(ai, 10) - Number.parseInt(bi, 10);
      if (d !== 0) return d;
    } else if (an !== bn) {
      // Numeric identifiers have lower precedence than alphanumeric ones.
      return an ? -1 : 1;
    } else if (ai !== bi) {
      return ai < bi ? -1 : 1;
    }
  }
  return 0;
}

/** Semver-aware compare: negative if a < b, positive if a > b, 0 if equal. */
export function compareSemver(a: string, b: string): number {
  const pa = parseCore(a);
  const pb = parseCore(b);
  for (let i = 0; i < 3; i++) {
    if (pa.core[i] !== pb.core[i]) return pa.core[i] - pb.core[i];
  }
  return comparePrerelease(pa.pre, pb.pre);
}

/** Returns the semver core of a "webapp-vX.Y.Z" tag, or null if it is not one. */
export function parseWebappTag(tag: string): string | null {
  const trimmed = tag.trim();
  if (!WEBAPP_TAG_RE.test(trimmed)) return null;
  return trimmed.slice('webapp-v'.length);
}

/**
 * Picks the highest stable webapp release from a GitHub releases list. Drafts,
 * prereleases, and every other component's tags (firmware-v*, protocol-v*,
 * sim-v*) are ignored, so this is safe against the repo's multi-component tag
 * stream. Returns null when there is no usable webapp release.
 */
export function pickLatestWebappRelease(releases: GithubRelease[]): WebappRelease | null {
  let best: WebappRelease | null = null;
  for (const r of releases) {
    if (r.draft || r.prerelease) continue;
    const version = parseWebappTag(r.tag_name);
    if (!version) continue;
    if (!best || compareSemver(version, best.version) > 0) {
      const tag = r.tag_name.trim();
      best = {
        tag,
        version,
        htmlUrl: r.html_url ?? releaseHtmlUrl(GITHUB_OWNER, GITHUB_REPO, tag),
      };
    }
  }
  return best;
}

/**
 * Base URL electron-updater's generic provider fetches its feed from. GitHub
 * release assets live under a stable per-tag directory, so pointing the
 * generic provider here makes it read latest.yml / latest-linux.yml (and the
 * referenced installer) from exactly the resolved webapp release, rather than
 * the github provider's "newest release in the repo" guess, which would land
 * on a firmware release that has no desktop feed.
 */
export function feedBaseUrl(owner: string, repo: string, tag: string): string {
  return `https://github.com/${owner}/${repo}/releases/download/${tag}/`;
}

/** Human-facing GitHub release page for the manual/open-in-browser path. */
export function releaseHtmlUrl(owner: string, repo: string, tag: string): string {
  return `https://github.com/${owner}/${repo}/releases/tag/${tag}`;
}

/**
 * Decides whether the current install can self-update. `platform` is
 * process.platform; `env` is process.env (only APPIMAGE is read). On Linux the
 * APPIMAGE variable is set exactly when the app runs from an AppImage, which is
 * the only self-updating Linux format we ship; deb and pacman installs are
 * owned by the system package manager. Windows (NSIS) always self-updates.
 * macOS is treated as manual because our dmgs are unsigned and Squirrel.Mac
 * refuses to apply an update it cannot validate.
 */
export function updateCapability(
  platform: NodeJS.Platform,
  env: Record<string, string | undefined>,
): UpdateCapability {
  if (platform === 'win32') return 'in-app';
  if (platform === 'linux') return env.APPIMAGE ? 'in-app' : 'manual';
  return 'manual';
}

/**
 * Combines the resolved latest release, the running version, and the install
 * capability into a single decision. Returns 'up-to-date' when the latest
 * release is not strictly newer than the current version (so an up-to-date app
 * never nags the user).
 */
export function decideUpdate(params: {
  currentVersion: string;
  latest: WebappRelease | null;
  capability: UpdateCapability;
}): UpdateDecision {
  const { currentVersion, latest, capability } = params;
  if (!latest) return { kind: 'up-to-date' };
  if (compareSemver(latest.version, currentVersion) <= 0) return { kind: 'up-to-date' };
  if (capability === 'in-app') {
    return {
      kind: 'in-app',
      version: latest.version,
      tag: latest.tag,
      feedUrl: feedBaseUrl(GITHUB_OWNER, GITHUB_REPO, latest.tag),
    };
  }
  return { kind: 'manual', version: latest.version, releaseUrl: latest.htmlUrl };
}
