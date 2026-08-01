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

// THE RULE: every commitlint scope whose commits can change code compiled
// into the ESP firmware image releases firmware. That is components/* and
// main/* (the ESP32-S3 target), plus nrf/* for the nRF52840 target, which
// shares the same components/ and main/ sources for everything except its
// platform shim: a change under a shared file changes both firmware images,
// so a scope used for nrf-side work is treated as firmware-code even when
// most of its history is ESP-only, and vice versa.
//
// This list was last audited 2026-08-01 against actual file paths touched
// by historical commits under every scope in commitlint.config.cjs's
// scope-enum (`git show --name-only` per scope, not guessed from the scope
// name). PR #386 added `protocol` after fix(protocol) receipt-storm fixes
// shipped with no firmware release; PR #392 shipped fix(location)/
// feat(gps)/feat(rpc) bullets that changed ESP-fleet behavior and also cut
// no release. This audit closes the rest of that gap in one pass: every
// scope below has at least one historical feat/fix/perf commit that touched
// components/, main/, or nrf/ files. When a new scope is added to
// commitlint's scope-enum, decide which list it belongs in using the same
// method (check what its commits actually touch) rather than the name
// alone; several scope names here are misleading (`hardware` turned out to
// mean PCB/KiCad, not firmware, while `chat`/`map`/`nodes`/`settings` turned
// out to mean the on-device T-Deck LVGL screens, not the webapp pages their
// names suggest).
//
// Deliberately EXCLUDED, with the reason another scope name might suggest
// otherwise:
//   - api: the api/openapi.yaml RPC contract file and its generated webapp
//     types, not compiled code. One historical outlier
//     (8d4292614, feat(api): add traffic debug rpc methods) touched
//     main/rpc_methods.c directly, but that predates the `rpc` scope's
//     current use for exactly this kind of change; new RPC method work now
//     uses `rpc` (included below), and a change to the contract alone does
//     not change what ships on a device.
//   - hardware: PCB/schematic/KiCad and hardware-docs work in current
//     usage (README hardware table, KiCad file hygiene). Two ancient
//     exceptions (#7, #22, both pre-dating this repo's scope-enum
//     maturing) fixed a battery driver bug under this scope; that class of
//     fix now lands under `peripherals` (included below), which is where
//     components/battery, components/button, and components/indicators
//     driver work is actually scoped today.
//   - anchor: 100% webapp (six commits, six times touching only
//     webapp/src/**); the on-device anchor/attestation protocol code lands
//     under `security`, `rpc`, or `protocol`, all included below.
//   - mock: webapp/mock is a standalone dev-only mock RPC server with its
//     own package.json, never part of a shipped build.
//   - go-public: one-off internal-refs redaction work, not a feature scope.
//   - emu, emu_link, emulator, emulator-qemu: the IDF linux target and its
//     QEMU/emu-link harness. Every dual-build component in this repo
//     (components/radio, components/display, components/gps,
//     components/button, components/battery, components/indicators) gates
//     its linux-target sources behind `if(${target} STREQUAL "linux")` in
//     CMakeLists.txt; those sources, and emu_link itself, are registered
//     header-only (no sources) on every other target, so an esp32s3 or nrf
//     build is provably unaffected by a commit scoped to any of these four.
//   - gosim, sim, sim-ui: the Go mesh simulator and its browser UI
//     (simulator/gosim, simulator/ui), released separately by
//     .releaserc.sim.cjs.
//   - host, test, e2e, smoke, verify, quality, firmware-quality,
//     firmware-build, sdd, versioning, docs, readme, ci, build, scripts,
//     tooling, release, web-flasher: test suites, CI gates, documentation,
//     and release tooling, none of which ship inside a firmware image.
//     (`build` looked promising at first glance: two of its four historical
//     commits are real feat/fix changes to the ESP-IDF build tree, but the
//     scope's current and predominant use is CI/build-script plumbing, and
//     splitting build-system commits from build-tooling ones by content
//     rather than name would need a scope split this audit is not making.)
//   - electron, webapp: the desktop/browser client, released by
//     .releaserc.webapp.cjs.
//   - deps, deps-dev: per repo policy these never cut a component release.
const FIRMWARE_SCOPES = [
  'firmware',
  // Mesh protocol implementation (receipt timing, reliability, routing
  // behavior) and on-device UI (T-Deck LVGL screens); see PR #386 for why
  // `protocol` is here and the `ui` comment on prior versions of this file
  // for why `ui` is.
  'protocol', 'ui',
  // Radio, mesh, and transport layer.
  'airtime', 'ble', 'channel', 'channels', 'crypto', 'delivery', 'flooding',
  'mesh', 'msg_store', 'network_key', 'packet', 'radio', 'reliability',
  'routing', 'timesync', 'wifi', 'ws',
  // Messaging features (dm, chat) and the identity/security layer under
  // them.
  'auth', 'dm', 'chat', 'identity', 'security',
  // Location and GNSS.
  'gps', 'location', 'map',
  // Storage and lifecycle.
  'mailbox', 'ota', 'persistence',
  // RPC and diagnostics surface.
  'probe', 'rpc', 'traffic_debug',
  // Board bring-up and peripherals: sdkconfig/board profiles, and the
  // real-hardware halves of the dual-build driver components (battery,
  // button, gps, indicators, display all gate a virtual backend onto the
  // linux target and a real one onto the device; see the `peripherals`
  // entry in components/gps/CMakeLists.txt for the pattern).
  'board', 'build', 'display', 'peripherals', 'sleep', 'touch',
  // Per-board profiles: each of these scopes' commits land in
  // main/boards/* or a board-specific driver.
  'heltec-v4', 'pager', 'tdeck',
  // The nRF52840 target: shares components/ and main/ with the ESP32-S3
  // build for everything except its platform shim, so nrf-scoped commits
  // routinely change ESP-fleet-shared code.
  'nrf',
  // On-device UI screens beyond the general `ui` scope: chat, map, nodes,
  // and settings are, in this repo's actual usage, the T-Deck LVGL screen
  // scopes (components/ui_graphics/screens/scr_*.c), not webapp pages, and
  // ui_gfx is an older name for the same component predating that split.
  'nodes', 'settings', 'ui_gfx', 'ui_graphics',
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

const scopeAlternation = FIRMWARE_SCOPES.join('|');

module.exports = {
  branches: ['main'],
  tagFormat: 'firmware-v${version}',
  plugins: [
    [squashExpander, {
      _wrapped: wrapped,
      // Wrapped commit-analyzer options. Scope-gated: every scope in
      // FIRMWARE_SCOPES releases firmware; see the audit comment above for
      // how that list was derived and what was deliberately left out.
      preset: 'conventionalcommits',
      releaseRules: [
        ...releaseRulesFor(FIRMWARE_SCOPES),
        // Suppress the preset default rules for any OTHER scope so an
        // out-of-scope fix/feat never leaks a firmware release. The negated
        // glob is deliberate: a plain { scope: '*', release: false } would
        // also match firmware-scoped commits, and commit-analyzer treats a
        // matched release:false as the highest-priority match (its index in
        // the release-type table is -1), so it would shadow the specific
        // per-scope rules above and suppress every firmware release.
        // Matching only out-of-scope scopes returns `false` for them
        // (blocking the default-rule fallback) while leaving in-scope
        // commits to the specific rules.
        { scope: `!(${scopeAlternation})`, release: false },
      ],
      // Wrapped release-notes-generator options: only list firmware-scoped
      // commits so the GitHub release notes stay component-specific.
      writerOpts: {
        transform: (commit) => {
          if (!commit.scope) return;
          const scopes = commit.scope.split(',');
          if (!scopes.some((s) => FIRMWARE_SCOPES.includes(s))) return;
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
