// Commitlint config for bramble. Enforced on pull requests by
// .github/workflows/commit-msg-lint.yml, which lints every commit in the PR
// plus the PR title (GitHub squash-merges use the PR title as the commit
// subject that semantic-release later versions from).
//
// Conventional Commits with a bramble-specific scope enum. The scope list is
// the union of the release-driving component scopes and the scopes actually
// used across the repo's history (>= 2 uses), so real work is not blocked.
// Scope is optional: plain typed commits like `docs: ...` are allowed.
module.exports = {
  extends: ['@commitlint/config-conventional'],
  rules: {
    // Longer headers: component-scoped subjects with a trailing "(#NNN)"
    // squash suffix run past the stock 100-char limit.
    'header-max-length': [2, 'always', 120],
    // Commit bodies and footers carry wrapped prose, URLs, and trailers that
    // routinely exceed the stock 100-char line limit; do not gate on them.
    'body-max-line-length': [0, 'always', Infinity],
    'footer-max-line-length': [0, 'always', Infinity],
    // No case gating on subjects. The release machinery (squash-expander +
    // semantic-release) parses type(scope) from squash-body bullets and never
    // reads capitalization, and commitlint's case detector disagrees with
    // itself across versions for subjects starting with an all-caps token
    // ("UF2 ...", "UART ...", "FreeRTOS ..."), which are common and correct
    // in embedded prose. The rule cost a branch rebuild for zero signal.
    'subject-case': [0],
    // Scope is optional (empty allowed) but, when present, must be known.
    'scope-enum': [
      2,
      'always',
      [
        'airtime',
        'anchor',
        'api',
        'auth',
        'battery',
        'ble',
        'board',
        'build',
        'channel',
        'channels',
        'chat',
        'ci',
        'crypto',
        'delivery',
        'display',
        'dm',
        'docs',
        'e2e',
        'electron',
        'emu',
        'emu_link',
        'emulator',
        'emulator-qemu',
        'firmware',
        'firmware-build',
        'firmware-quality',
        'flooding',
        'go-public',
        'gosim',
        'gps',
        'hardware',
        'heltec-v4',
        'host',
        'identity',
        'location',
        'mailbox',
        'map',
        'mesh',
        'mock',
        'msg_store',
        'network_key',
        'nodes',
        'nrf',
        'ota',
        'packet',
        'pager',
        'peripherals',
        'persistence',
        'probe',
        'protocol',
        'quality',
        'radio',
        'readme',
        'release',
        'reliability',
        'routing',
        'rpc',
        'scripts',
        'sdd',
        'security',
        'settings',
        'sim',
        'sim-ui',
        'sleep',
        'smoke',
        'test',
        'tdeck',
        'timesync',
        'tooling',
        'touch',
        'trackball',
        'traffic_debug',
        'ui',
        'ui_gfx',
        'ui_graphics',
        'verify',
        'versioning',
        'web-flasher',
        'webapp',
        'wifi',
        'ws',
        'deps',
        'deps-dev',
      ],
    ],
  },
};
