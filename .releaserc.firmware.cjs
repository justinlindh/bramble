const giteaUrl = process.env.GITEA_URL || 'https://github.com';

module.exports = {
  branches: ['main'],
  tagFormat: 'firmware-v${version}',
  plugins: [
    [
      '@semantic-release/commit-analyzer',
      {
        preset: 'conventionalcommits',
        releaseRules: [
          { breaking: true, scope: 'firmware', release: 'major' },
          { revert: true, scope: 'firmware', release: 'patch' },
          { type: 'feat', scope: 'firmware', release: 'minor' },
          { type: 'fix', scope: 'firmware', release: 'patch' },
          { type: 'perf', scope: 'firmware', release: 'patch' },
          { scope: '*', release: false }
        ]
      }
    ],
    '@semantic-release/release-notes-generator',
    [
      '@saithodev/semantic-release-gitea',
      { giteaUrl }
    ]
  ]
};
