# CI Smoke Workflow

This workflow validates that:
1. Gitea Actions runner is online and can execute jobs
2. Artifact upload works in Gitea Actions

Workflow file: `.gitea/workflows/ci-smoke-artifacts.yml`

## Manual verification
- Trigger workflow in Gitea UI
- Confirm successful run
- Confirm `ci-smoke-artifact` is downloadable from run artifacts

Bring-up completed 2026-02-24; kept as a runner health check for future
runner or infra changes.
