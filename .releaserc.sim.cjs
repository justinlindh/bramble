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

// THE RULE (audited 2026-08-01 alongside .releaserc.firmware.cjs, same
// method: check what historical commits under each scope actually
// touched): every scope whose commits change simulator/* releases sim.
// `gosim` (simulator/gosim, the Go mesh engine) and `sim-ui` (simulator/ui,
// its browser front end) are both used exclusively for work inside
// simulator/, the exact directory tree this config releases, but neither
// was in the original single-scope rule, so real simulator commits under
// those names cut no sim release. Not added: `emulator`/`emu`/`emu_link`/
// `emulator-qemu` (the IDF linux-target harness under emulator/, a
// different directory with no release config of its own) and `reliability`
// (its scope is dominated by firmware work; the rare commit that also
// touches simulator/engine as a side effect does not warrant folding a
// firmware-implementation scope into this config).
const SIM_SCOPES = ['sim', 'gosim', 'sim-ui'];

function releaseRulesFor(scopes) {
  return scopes.flatMap((scope) => [
    { breaking: true, scope, release: 'major' },
    { revert: true, scope, release: 'patch' },
    { type: 'feat', scope, release: 'minor' },
    { type: 'fix', scope, release: 'patch' },
    { type: 'perf', scope, release: 'patch' },
  ]);
}

const scopeAlternation = SIM_SCOPES.join('|');

module.exports = {
  branches: ['main'],
  tagFormat: 'sim-v${version}',
  plugins: [
    [squashExpander, {
      _wrapped: wrapped,
      // Wrapped commit-analyzer options. Scope-gated: every scope in
      // SIM_SCOPES releases sim; see the audit comment above.
      preset: 'conventionalcommits',
      releaseRules: [
        ...releaseRulesFor(SIM_SCOPES),
        // Suppress the preset default rules for any OTHER scope so an
        // out-of-scope fix/feat never leaks a sim release. The negated glob
        // is deliberate: a plain { scope: '*', release: false } would also
        // match sim-scoped commits, and commit-analyzer treats a matched
        // release:false as the highest-priority match (its index in the
        // release-type table is -1), so it would shadow the specific sim
        // rules above and suppress every sim release. Matching only non-sim
        // scopes returns `false` for out-of-scope commits (blocking the
        // default-rule fallback) while leaving in-scope commits to the
        // specific rules.
        { scope: `!(${scopeAlternation})`, release: false }
      ],
      // Wrapped release-notes-generator options: only list sim-scoped
      // commits so the GitHub release notes stay component-specific.
      writerOpts: {
        transform: (commit) => {
          if (!commit.scope) return;
          const scopes = commit.scope.split(',');
          if (!scopes.some((s) => SIM_SCOPES.includes(s))) return;
          const typeMap = { feat: 'Features', fix: 'Bug Fixes', perf: 'Performance Improvements' };
          if (!typeMap[commit.type]) return;
          return { ...commit, type: typeMap[commit.type], shortHash: commit.hash && commit.hash.substring(0, 7) };
        },
      },
    }],
    ['@semantic-release/github', { successComment: false, failComment: false, failTitle: false }]
  ]
};
