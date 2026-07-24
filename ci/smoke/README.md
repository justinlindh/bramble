# CI Smoke Workflow

This workflow validates that:

1. The runner pool is online and can pick up a job
2. Artifact upload works on that runner

Workflow file: `.github/workflows/ci-smoke-artifacts.yml`

## Manual verification

- Run the workflow from the GitHub Actions UI (`workflow_dispatch`)
- Confirm successful run
- Confirm `ci-smoke-artifact` is downloadable from run artifacts

It exercises no repo code and gates nothing; it is the cheap probe to run
after a runner or infra change, in place of dispatching a full build.
