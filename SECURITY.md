# Security Policy

Bramble is a cryptographic mesh protocol. Its threat model, the protections it
claims, and the gaps it knows about are documented in
[docs/SECURITY-MODEL.md](docs/SECURITY-MODEL.md). Reading that first is worth
your time: it may already describe the behavior you are looking at, including
several residual weaknesses that are deliberate and stated rather than
overlooked.

## Reporting a vulnerability

**Please do not open a public issue for a security vulnerability.**

Use GitHub's private vulnerability reporting, which is enabled on this
repository:

1. Go to <https://github.com/justinlindh/bramble/security/advisories/new>
2. Or, from the repository page: **Security** tab, then **Report a
   vulnerability**.

That opens a private advisory visible only to you and the maintainers, and it
gives us a place to work on a fix and coordinate disclosure with you.

If you cannot use GitHub for some reason, the project contact address is
`info@bramblemesh.org`.

## What to include

The more of this you can provide, the faster a report turns into a fix:

- What the issue is and what an attacker gains from it.
- Where in the tree it lives: component, file, and ideally the function.
- How to reproduce it. A host test in `test/`, a simulator scenario in
  `simulator/scenarios/`, or an emulator scenario is the ideal form, since
  those run without hardware and become the regression test.
- Which version, commit, or firmware build you observed it on, and on what
  (real board, emulator, simulator, host tests).
- Whether the attack needs the network key, physical access to a device, or
  neither. That distinction usually determines severity here, because a
  provisioned Bramble mesh treats every network-key holder as an insider.

## What happens next

This is a small project, currently maintained by one person, so treat the
following as intent rather than a service commitment: reports are triaged as
soon as is practical, you will get an acknowledgement in the advisory thread,
and we will keep you updated on whether the report is accepted and what the
fix looks like. If you have a disclosure deadline in mind, say so in the
report and we will tell you honestly whether it is achievable.

Credit is given in the advisory and the release notes unless you ask us not
to.

## Scope

In scope: the firmware in `main/` and `components/`, the protocol itself, the
webapp and Electron desktop client in `webapp/`, the RPC surface described by
`api/openapi.yaml`, the simulator and emulator, and the build and release
tooling in `scripts/`.

Some things are known and documented rather than undisclosed. Before
reporting, check [docs/SECURITY-MODEL.md](docs/SECURITY-MODEL.md) for whether
what you found is one of the stated residuals. Examples of behavior that is
by design and documented there:

- Any holder of the network key is an insider and can forge control-plane
  MACs. Provisioning is the trust boundary.
- An un-anchored mesh has unforgeable but free-to-mint identities. The
  optional per-fleet trust anchor is what closes Sybil identity minting; see
  [docs/trust-anchor.md](docs/trust-anchor.md).
- Traffic timing and the fact that a node transmits are observable. Bramble
  protects content and reduces metadata exposure; it is not a traffic-analysis
  resistant network.

Reporting one of these is not a problem, but saying which documented residual
you think is understated, and why, makes the report far more useful than a
rediscovery.

## Supported versions

Bramble is pre-1.0 and under active development. Fixes land on `main` and go
out in the next component release. There are no long-term support branches,
so the practical advice is to track the latest release.
