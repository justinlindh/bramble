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
        // Suppress the preset default rules for any OTHER scope so an
        // out-of-scope fix/feat never leaks a webapp release. The negated
        // glob is deliberate: a plain { scope: '*', release: false } would
        // also match webapp-scoped commits, and commit-analyzer treats a
        // matched release:false as the highest-priority match (its index in
        // the release-type table is -1), so it would shadow the specific
        // webapp rules above and suppress every webapp release. Matching
        // only non-webapp scopes returns `false` for out-of-scope commits
        // (blocking the default-rule fallback) while leaving in-scope commits
        // to the specific rules.
        { scope: '!(webapp)', release: false }
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
    // draftRelease: create the GitHub release as a draft, not published. The
    // desktop-installer jobs build the AppImage/deb/pacman/exe/dmg after
    // semantic-release runs and attach them to this draft, then flip it to
    // published. GitHub releases are immutable once published: assets cannot be
    // added afterward (the REST API rejects the upload with HTTP 422), so the
    // release must stay a mutable draft until its installers are attached.
    // semantic-release core still creates and pushes the webapp-v* git tag
    // regardless of the draft flag, so the tag-diff publish detection and the
    // installer jobs' tag checkout are unaffected.
    ['@semantic-release/github', { successComment: false, failComment: false, failTitle: false, draftRelease: true }]
  ]
};
