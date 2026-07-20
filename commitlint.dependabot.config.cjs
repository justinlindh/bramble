// Relaxed commitlint config used ONLY for dependabot[bot] pull requests. See
// docs/ci.md and issue #190.
//
// Dependabot's grouped-update PR title template is
// "bump X from A to B in DIR in the GROUP group across N directories", and
// that template is Dependabot's own, not something a commit author controls.
// .github/dependabot.yml now keeps GROUP as short as it can reasonably be
// ("patch", "gha"), which comfortably fixes the common case, but a single
// long npm package name/version pair (a scoped devDependency, for example)
// can still push a grouped title past 120 chars even with the shortest
// defensible group name. Raising header-max-length for every human commit to
// cover a bot's title generator would weaken a rule that exists to keep
// `git log` readable, since the PR title becomes the squash-commit subject.
//
// This file changes exactly one rule, header-max-length, to a bound that is
// generous but still a bound, not "no limit for bots." Every other rule,
// type-enum and scope-enum included, stays identical to the real config by
// extending it, so a Dependabot title with an invalid type or an unknown
// scope still fails the check same as it would for a human author.
//
// Selected by .github/workflows/commit-msg-lint.yml ONLY when
// github.event.pull_request.user.login == 'dependabot[bot]', the same actor
// gate pr-template.yml already uses for the same bot. That field is set by
// GitHub from the PR's actual author, so a contributor cannot spoof it by
// writing a Dependabot-shaped title themselves.
const base = require('./commitlint.config.cjs');

module.exports = {
  ...base,
  rules: {
    ...base.rules,
    // 160 is a deliberate, bounded allowance, not an escalation path: if a
    // future grouped title exceeds it, that is a signal to look at why
    // (a new ecosystem, a longer package name pattern), not to raise this
    // number again without writing down the reason (see docs/quality-policy.md's
    // change-management rule).
    'header-max-length': [2, 'always', 160],
  },
};
