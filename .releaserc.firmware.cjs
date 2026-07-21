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
      // Wrapped commit-analyzer options. Scope-gated: commits scoped to
      // firmware release firmware, and so do commits scoped to ui, because
      // scope `ui` is DEVICE-UI code (components/ui_graphics, the T-Deck
      // LVGL screens) that ships inside the firmware image; without this a
      // user-visible on-device UI fix sat unreleased until an unrelated
      // firmware-scoped commit happened to land.
      preset: 'conventionalcommits',
      releaseRules: [
        { breaking: true, scope: 'firmware', release: 'major' },
        { revert: true, scope: 'firmware', release: 'patch' },
        { type: 'feat', scope: 'firmware', release: 'minor' },
        { type: 'fix', scope: 'firmware', release: 'patch' },
        { type: 'perf', scope: 'firmware', release: 'patch' },
        { breaking: true, scope: 'ui', release: 'major' },
        { revert: true, scope: 'ui', release: 'patch' },
        { type: 'feat', scope: 'ui', release: 'minor' },
        { type: 'fix', scope: 'ui', release: 'patch' },
        { type: 'perf', scope: 'ui', release: 'patch' },
        // Suppress the preset default rules for any OTHER scope so an
        // out-of-scope fix/feat never leaks a firmware release. The negated
        // glob is deliberate: a plain { scope: '*', release: false } would
        // also match firmware-scoped commits, and commit-analyzer treats a
        // matched release:false as the highest-priority match (its index in
        // the release-type table is -1), so it would shadow the specific
        // firmware and ui rules above and suppress every firmware release.
        // Matching only out-of-scope scopes returns `false` for them
        // (blocking the default-rule fallback) while leaving in-scope commits
        // to the specific rules.
        { scope: '!(firmware|ui)', release: false }
      ],
      // Wrapped release-notes-generator options: only list firmware- and
      // ui-scoped commits so the GitHub release notes stay component-specific.
      writerOpts: {
        transform: (commit) => {
          if (!commit.scope || !/(^|,)(firmware|ui)(,|$)/.test(commit.scope)) return;
          const typeMap = { feat: 'Features', fix: 'Bug Fixes', perf: 'Performance Improvements' };
          if (!typeMap[commit.type]) return;
          return { ...commit, type: typeMap[commit.type], shortHash: commit.hash && commit.hash.substring(0, 7) };
        },
      },
    }],
    // draftRelease: create the GitHub release as a draft, not published. The
    // release job builds the per-board factory images after semantic-release
    // runs and attaches them to this draft, then flips it to published. GitHub
    // releases are immutable once published: assets cannot be added afterward
    // (the REST API rejects the upload with HTTP 422), so the release must stay
    // a mutable draft until its factory images are attached. semantic-release
    // core still creates and pushes the firmware-v* git tag regardless of the
    // draft flag, so the tag-diff publish detection in the workflow is unaffected.
    ['@semantic-release/github', { successComment: false, failComment: false, failTitle: false, draftRelease: true }]
  ]
};
