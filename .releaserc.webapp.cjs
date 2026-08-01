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

// THE RULE: every commitlint scope whose commits can change code shipped in
// the webapp build (webapp/src, webapp/electron, the React app and its
// Electron desktop shell) releases webapp. Audited 2026-08-01 as part of
// the firmware-release-scope audit (see .releaserc.firmware.cjs for the
// full method and the shared exclusions it documents, e.g. mock, api,
// deps/deps-dev, test/CI/tooling scopes). The same historical-commit check
// found that several feature-named scopes are used almost entirely for
// webapp/src work even though nothing about their name says so:
//   - anchor: trust-anchor enrollment UI (webapp/src/pages/Config/
//     AnchorSection*, webapp/src/utils/anchor*), six commits, all webapp,
//     zero device code.
//   - electron: webapp/electron/main.ts, the desktop shell.
// A second group is genuinely dual-scoped between webapp and firmware (a
// single commit changing both a webapp screen and the on-device code it
// talks to): chat, channels, dm, nodes, map, ota, persistence, probe, rpc,
// security, tdeck, traffic_debug, ws. These are also in FIRMWARE_SCOPES in
// .releaserc.firmware.cjs; the two configs share a scope name whenever a
// commit under it can plausibly touch either build.
//
// Deliberately left out despite some webapp-directory overlap: `settings`
// and `ui_gfx`/`ui_graphics` (found to be exclusively the T-Deck on-device
// UI, not webapp, see the firmware config's comment); `heltec-v4`,
// `protocol`, and `firmware` (one or two incidental webapp-side touches in
// an otherwise firmware-only history, not a recurring pattern); `release`
// and `quality` (release/CI tooling, would be circular to include here).
const WEBAPP_SCOPES = [
  'webapp',
  // Pure webapp features.
  'anchor', 'electron',
  // Dual-scoped: also released by .releaserc.firmware.cjs when the same
  // commit changes on-device code.
  'chat', 'channels', 'dm', 'map', 'nodes', 'ota', 'persistence', 'probe',
  'rpc', 'security', 'tdeck', 'traffic_debug', 'ws',
];

function releaseRulesFor(scopes) {
  return scopes.flatMap((scope) => [
    { breaking: true, scope, release: 'major' },
    { revert: true, scope, release: 'patch' },
    { type: 'feat', scope, release: 'minor' },
    { type: 'fix', scope, release: 'patch' },
    { type: 'perf', scope, release: 'patch' },
  ]);
}

const scopeAlternation = WEBAPP_SCOPES.join('|');

module.exports = {
  branches: ['main'],
  tagFormat: 'webapp-v${version}',
  plugins: [
    [squashExpander, {
      _wrapped: wrapped,
      // Wrapped commit-analyzer options. Scope-gated: every scope in
      // WEBAPP_SCOPES releases webapp; see the audit comment above.
      preset: 'conventionalcommits',
      releaseRules: [
        ...releaseRulesFor(WEBAPP_SCOPES),
        // Suppress the preset default rules for any OTHER scope so an
        // out-of-scope fix/feat never leaks a webapp release. The negated
        // glob is deliberate: a plain { scope: '*', release: false } would
        // also match webapp-scoped commits, and commit-analyzer treats a
        // matched release:false as the highest-priority match (its index in
        // the release-type table is -1), so it would shadow the specific
        // per-scope rules above and suppress every webapp release. Matching
        // only non-webapp scopes returns `false` for out-of-scope commits
        // (blocking the default-rule fallback) while leaving in-scope commits
        // to the specific rules.
        { scope: `!(${scopeAlternation})`, release: false },
      ],
      // Wrapped release-notes-generator options: only list webapp-scoped
      // commits so the GitHub release notes stay component-specific.
      writerOpts: {
        transform: (commit) => {
          if (!commit.scope) return;
          const scopes = commit.scope.split(',');
          if (!scopes.some((s) => WEBAPP_SCOPES.includes(s))) return;
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
