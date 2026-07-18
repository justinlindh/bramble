// Simulator semantic-release config.
//
// Uses scripts/release/semantic-release-squash-expander instead of the
// standard commit-analyzer / release-notes-generator pair so GitHub
// squash-merge commits get expanded into their per-bullet virtual commits
// before scope matching. Without this, a cross-component PR squashed under
// (e.g.) scope "firmware" would never trigger this sim-scoped release even
// if the squash body contained "fix(sim): ..." bullets.
//
// Publishes to GitHub with @semantic-release/github (default GITHUB_TOKEN
// auth).
const path = require('path');
const squashExpander = path.resolve(__dirname, 'scripts', 'release', 'semantic-release-squash-expander.cjs');
const wrapped = {
  commitAnalyzer: require('@semantic-release/commit-analyzer'),
  releaseNotesGenerator: require('@semantic-release/release-notes-generator'),
};

module.exports = {
  branches: ['main'],
  tagFormat: 'sim-v${version}',
  plugins: [
    [squashExpander, {
      _wrapped: wrapped,
      // Wrapped commit-analyzer options. Scope-gated: only commits scoped
      // to sim release sim.
      preset: 'conventionalcommits',
      releaseRules: [
        { breaking: true, scope: 'sim', release: 'major' },
        { revert: true, scope: 'sim', release: 'patch' },
        { type: 'feat', scope: 'sim', release: 'minor' },
        { type: 'fix', scope: 'sim', release: 'patch' },
        { type: 'perf', scope: 'sim', release: 'patch' },
        { scope: '*', release: false }
      ],
      // Wrapped release-notes-generator options: only list sim-scoped
      // commits so the GitHub release notes stay component-specific.
      writerOpts: {
        transform: (commit) => {
          if (!commit.scope || !/(^|,)sim(,|$)/.test(commit.scope)) return;
          const typeMap = { feat: 'Features', fix: 'Bug Fixes', perf: 'Performance Improvements' };
          if (!typeMap[commit.type]) return;
          return { ...commit, type: typeMap[commit.type], shortHash: commit.hash && commit.hash.substring(0, 7) };
        },
      },
    }],
    '@semantic-release/github'
  ]
};
