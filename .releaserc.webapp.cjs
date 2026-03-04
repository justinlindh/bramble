const giteaUrl = process.env.GITEA_URL || 'https://git.idiotica.org';

module.exports = {
  branches: ['main'],
  tagFormat: 'webapp-v${version}',
  plugins: [
    [
      '@semantic-release/commit-analyzer',
      {
        preset: 'conventionalcommits',
        releaseRules: [
          { breaking: true, scope: 'webapp', release: 'major' },
          { revert: true, scope: 'webapp', release: 'patch' },
          { type: 'feat', scope: 'webapp', release: 'minor' },
          { type: 'fix', scope: 'webapp', release: 'patch' },
          { type: 'perf', scope: 'webapp', release: 'patch' },
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
