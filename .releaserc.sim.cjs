module.exports = {
  branches: ['main'],
  tagFormat: 'sim-v${version}',
  plugins: [
    [
      '@semantic-release/commit-analyzer',
      {
        preset: 'conventionalcommits',
        releaseRules: [
          { breaking: true, scope: 'sim', release: 'major' },
          { revert: true, scope: 'sim', release: 'patch' },
          { type: 'feat', scope: 'sim', release: 'minor' },
          { type: 'fix', scope: 'sim', release: 'patch' },
          { type: 'perf', scope: 'sim', release: 'patch' },
          { scope: '*', release: false }
        ]
      }
    ],
    '@semantic-release/release-notes-generator'
  ]
};

// TODO(phase-3): enable simulator semantic-release job once sim packaging artifacts are finalized.
