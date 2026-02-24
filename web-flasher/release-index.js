(function (global) {
  const SUPPORTED_BOARDS = ['heltec-v3', 'tdeck-plus', 'heltec-v4'];
  const REQUIRED_FILES = ['bootloader.bin', 'partition-table.bin', 'bramble.bin'];

  function artifactNameFromPath(path) {
    return String(path || '').split('/').pop();
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

  function normalizeAndSortReleases(indexData) {
    if (!indexData || !Array.isArray(indexData.releases)) return [];
    return [...indexData.releases]
      .filter(r => r && r.version && r.published_at && Array.isArray(r.artifacts))
      .filter(isCompleteRelease)
      .sort((a, b) => {
        const dt = new Date(b.published_at) - new Date(a.published_at);
        if (dt !== 0) return dt;
        return String(b.version).localeCompare(String(a.version), undefined, { numeric: true, sensitivity: 'base' });
      });
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
    normalizeAndSortReleases,
    resolveArtifactsForBoardRelease,
    isCompleteRelease,
  };

  if (typeof module !== 'undefined' && module.exports) {
    module.exports = api;
  }
  global.BrambleReleaseIndex = api;
})(typeof window !== 'undefined' ? window : globalThis);
