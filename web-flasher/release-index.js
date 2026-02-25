(function (global) {
  const SUPPORTED_BOARDS = ['heltec-v3', 'tdeck-plus', 'heltec-v4'];
  const REQUIRED_FILES = ['bootloader.bin', 'partition-table.bin', 'bramble.bin'];
  const SEMVERISH_RE = /^v?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+([0-9A-Za-z.-]+))?$/;

  function artifactNameFromPath(path) {
    return String(path || '').split('/').pop();
  }

  function parseSemverish(version) {
    const raw = String(version || '').trim();
    const m = raw.match(SEMVERISH_RE);
    if (!m) return null;
    return {
      major: Number(m[1]),
      minor: Number(m[2]),
      patch: Number(m[3]),
      prerelease: m[4] || '',
      build: m[5] || '',
      raw,
    };
  }

  function normalizeVersionLabel(version) {
    const parsed = parseSemverish(version);
    if (!parsed) return null;
    return `v${parsed.major}.${parsed.minor}.${parsed.patch}${parsed.prerelease ? `-${parsed.prerelease}` : ''}${parsed.build ? `+${parsed.build}` : ''}`;
  }

  function hasCanonicalSet(artifacts, board) {
    const names = new Set(
      (artifacts || [])
        .filter(a => a && a.board === board)
        .map(a => artifactNameFromPath(a.file))
    );
    return REQUIRED_FILES.every((f) => names.has(f));
  }

  function isCompleteRelease(release) {
    if (!release || !Array.isArray(release.artifacts)) return false;
    return SUPPORTED_BOARDS.every((board) => hasCanonicalSet(release.artifacts, board));
  }

  function compareSemverishDesc(a, b) {
    if (a.major !== b.major) return b.major - a.major;
    if (a.minor !== b.minor) return b.minor - a.minor;
    if (a.patch !== b.patch) return b.patch - a.patch;
    if (!!a.prerelease !== !!b.prerelease) return a.prerelease ? 1 : -1;
    return b.prerelease.localeCompare(a.prerelease, undefined, { numeric: true, sensitivity: 'base' });
  }

  function normalizeAndSortReleases(indexData) {
    if (!indexData || !Array.isArray(indexData.releases)) return [];
    return [...indexData.releases]
      .filter(r => r && r.version && r.published_at && Array.isArray(r.artifacts))
      .map((r) => {
        const displayVersion = normalizeVersionLabel(r.version);
        if (!displayVersion) return null;
        return { ...r, version: displayVersion, _parsedVersion: parseSemverish(displayVersion) };
      })
      .filter(Boolean)
      .filter(isCompleteRelease)
      .sort((a, b) => {
        const dt = new Date(b.published_at) - new Date(a.published_at);
        if (dt !== 0) return dt;
        const bySemver = compareSemverishDesc(a._parsedVersion, b._parsedVersion);
        if (bySemver !== 0) return bySemver;
        return String(b.version).localeCompare(String(a.version), undefined, { numeric: true, sensitivity: 'base' });
      })
      .map(({ _parsedVersion, ...r }) => r);
  }

  function resolveArtifactsForBoardRelease(board, release, boardConfig) {
    if (!release || !boardConfig || !Array.isArray(boardConfig.partitions)) {
      throw new Error('Invalid board/release configuration');
    }

    const boardArtifacts = (release.artifacts || []).filter(a => a.board === board);
    if (boardArtifacts.length === 0) {
      throw new Error(`No artifacts for selected board in release ${release.version}`);
    }

    const resolved = {};
    for (const part of boardConfig.partitions) {
      const match = boardArtifacts.find(a => artifactNameFromPath(a.file) === part.file);
      if (!match) {
        throw new Error(`Release ${release.version} missing ${part.file} for selected board`);
      }
      resolved[part.file] = match;
    }
    return resolved;
  }

  const api = {
    SUPPORTED_BOARDS,
    REQUIRED_FILES,
    normalizeVersionLabel,
    normalizeAndSortReleases,
    resolveArtifactsForBoardRelease,
    isCompleteRelease,
  };

  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  }
  global.BrambleReleaseIndex = api;
})(typeof window !== 'undefined' ? window : globalThis);
