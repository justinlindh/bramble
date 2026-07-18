// Webapp semantic-release config.
//
// Uses scripts/release/semantic-release-squash-expander instead of the
// standard commit-analyzer / release-notes-generator pair so GitHub
// squash-merge commits get expanded into their per-bullet virtual commits
// before scope matching. Without this, a cross-component PR squashed under
// (e.g.) scope "firmware" would never trigger this webapp-scoped release
// even if the squash body contained "fix(webapp): ..." bullets.
//
// Publishes to GitHub with @semantic-release/github (default GITHUB_TOKEN
// auth). The retired Gitea mirror and its RELEASE_FORGE_URL requirement are
// gone.
const path = require('path');
const squashExpander = path.resolve(__dirname, 'scripts', 'release', 'semantic-release-squash-expander.cjs');
const wrapped = {
  commitAnalyzer: require('@semantic-release/commit-analyzer'),
  releaseNotesGenerator: require('@semantic-release/release-notes-generator'),
};

module.exports = {
  branches: ['main'],
  tagFormat: 'webapp-v${version}',
  plugins: [
    [squashExpander, {
      _wrapped: wrapped,
      // Wrapped commit-analyzer options. Scope-gated: only commits scoped
      // to webapp release webapp.
      preset: 'conventionalcommits',
      releaseRules: [
        { breaking: true, scope: 'webapp', release: 'major' },
        { revert: true, scope: 'webapp', release: 'patch' },
        { type: 'feat', scope: 'webapp', release: 'minor' },
        { type: 'fix', scope: 'webapp', release: 'patch' },
        { type: 'perf', scope: 'webapp', release: 'patch' },
        { scope: '*', release: false }
      ],
      // Wrapped release-notes-generator options: only list webapp-scoped
      // commits so the GitHub release notes stay component-specific.
      writerOpts: {
        transform: (commit) => {
          if (!commit.scope || !/(^|,)webapp(,|$)/.test(commit.scope)) return;
          const typeMap = { feat: 'Features', fix: 'Bug Fixes', perf: 'Performance Improvements' };
          if (!typeMap[commit.type]) return;
          return { ...commit, type: typeMap[commit.type], shortHash: commit.hash && commit.hash.substring(0, 7) };
        },
      },
    }],
    ['@semantic-release/github', { successComment: false, failComment: false, failTitle: false }]
  ]
};
