// Protocol semantic-release config.
//
// Uses scripts/release/semantic-release-squash-expander instead of the
// standard commit-analyzer / release-notes-generator pair so GitHub
// squash-merge commits get expanded into their per-bullet virtual commits
// before scope matching. Without this, a cross-component PR squashed under
// (e.g.) scope "firmware" would never trigger this protocol-scoped release
// even if the squash body contained "fix(protocol): ..." bullets.
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
  tagFormat: 'protocol-v${version}',
  plugins: [
    [squashExpander, {
      _wrapped: wrapped,
      // Wrapped commit-analyzer options. Scope-gated: only commits scoped
      // to protocol release protocol.
      preset: 'conventionalcommits',
      releaseRules: [
        { breaking: true, scope: 'protocol', release: 'major' },
        { revert: true, scope: 'protocol', release: 'patch' },
        { type: 'feat', scope: 'protocol', release: 'minor' },
        { type: 'fix', scope: 'protocol', release: 'patch' },
        { type: 'perf', scope: 'protocol', release: 'patch' },
        // Suppress the preset default rules for any OTHER scope so an
        // out-of-scope fix/feat never leaks a protocol release. The negated
        // glob is deliberate: a plain { scope: '*', release: false } would
        // also match protocol-scoped commits, and commit-analyzer treats a
        // matched release:false as the highest-priority match (its index in
        // the release-type table is -1), so it would shadow the specific
        // protocol rules above and suppress every protocol release. Matching
        // only non-protocol scopes returns `false` for out-of-scope commits
        // (blocking the default-rule fallback) while leaving in-scope commits
        // to the specific rules.
        { scope: '!(protocol)', release: false }
      ],
      // Wrapped release-notes-generator options: only list protocol-scoped
      // commits so the GitHub release notes stay component-specific.
      writerOpts: {
        transform: (commit) => {
          if (!commit.scope || !/(^|,)protocol(,|$)/.test(commit.scope)) return;
          const typeMap = { feat: 'Features', fix: 'Bug Fixes', perf: 'Performance Improvements' };
          if (!typeMap[commit.type]) return;
          return { ...commit, type: typeMap[commit.type], shortHash: commit.hash && commit.hash.substring(0, 7) };
        },
      },
    }],
    ['@semantic-release/github', { successComment: false, failComment: false, failTitle: false }]
  ]
};
