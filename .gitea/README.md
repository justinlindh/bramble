# .gitea (frozen mirror, not the CI you are looking for)

`.github/workflows/` is authoritative for CI. `.gitea/workflows/` is a frozen,
unmodified copy kept for the Gitea mirror, it is not edited when workflows
change, and the workflows in it can drift from the real ones. If you are
reading CI definitions, read `.github/workflows/`.

The two lint targets in the `Makefile` that still point actionlint at
`.gitea/workflows/*.yml` are deliberate: they keep the frozen copy
syntactically valid, they do not make it authoritative.

See the "Dual-tree arrangement" section of [`docs/ci.md`](../docs/ci.md) for
the full arrangement and the Phase 2 port plan.
