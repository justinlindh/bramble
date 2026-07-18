// Firmware semantic-release config.
//
// Uses scripts/release/semantic-release-squash-expander instead of the
// standard commit-analyzer / release-notes-generator pair so GitHub
// squash-merge commits get expanded into their per-bullet virtual commits
// before scope matching. Without this, a cross-component PR squashed under
// (e.g.) scope "webapp" would never trigger this firmware-scoped release
// even if the squash body contained "fix(firmware): ..." bullets.
//
// Publishes to GitHub with @semantic-release/github (default GITHUB_TOKEN
// auth). The retired Gitea mirror and its RELEASE_FORGE_URL requirement are
// gone.
const path = require('path');
const squashExpander = path.resolve(__dirname, 'scripts', 'release', 'semantic-release-squash-expander.cjs');
// The expander lives outside any project's node_modules and would fail to
// resolve these otherwise; load them here where they're installed and pass
// them through plugin options.
const wrapped = {
  commitAnalyzer: require('@semantic-release/commit-analyzer'),
  releaseNotesGenerator: require('@semantic-release/release-notes-generator'),
};

module.exports = {
  branches: ['main'],
  tagFormat: 'firmware-v${version}',
  plugins: [
    [squashExpander, {
      _wrapped: wrapped,
      // Wrapped commit-analyzer options. Scope-gated: only commits scoped
      // to firmware release firmware.
      preset: 'conventionalcommits',
      releaseRules: [
        { breaking: true, scope: 'firmware', release: 'major' },
        { revert: true, scope: 'firmware', release: 'patch' },
        { type: 'feat', scope: 'firmware', release: 'minor' },
        { type: 'fix', scope: 'firmware', release: 'patch' },
        { type: 'perf', scope: 'firmware', release: 'patch' },
        { scope: '*', release: false }
      ],
      // Wrapped release-notes-generator options: only list firmware-scoped
      // commits so the GitHub release notes stay component-specific.
      writerOpts: {
        transform: (commit) => {
          if (!commit.scope || !/(^|,)firmware(,|$)/.test(commit.scope)) return;
          const typeMap = { feat: 'Features', fix: 'Bug Fixes', perf: 'Performance Improvements' };
          if (!typeMap[commit.type]) return;
          return { ...commit, type: typeMap[commit.type], shortHash: commit.hash && commit.hash.substring(0, 7) };
        },
      },
    }],
    '@semantic-release/github'
  ]
};
