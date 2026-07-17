if (!process.env.GITEA_URL) {
  throw new Error('GITEA_URL is required (set it in the release environment)');
}
const giteaUrl = process.env.GITEA_URL;

module.exports = {
  branches: ['main'],
  tagFormat: 'protocol-v${version}',
  plugins: [
    [
      '@semantic-release/commit-analyzer',
      {
        preset: 'conventionalcommits',
        releaseRules: [
          { breaking: true, scope: 'protocol', release: 'major' },
          { revert: true, scope: 'protocol', release: 'patch' },
          { type: 'feat', scope: 'protocol', release: 'minor' },
          { type: 'fix', scope: 'protocol', release: 'patch' },
          { type: 'perf', scope: 'protocol', release: 'patch' },
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
