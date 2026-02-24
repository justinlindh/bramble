# Gitea Actions Runner Threat Model

## Key risk
Mounting `/var/run/docker.sock` gives job steps effectively root-equivalent control of host Docker.

## Boundary decisions
- Use a **dedicated runner** for trusted repos only.
- Restrict labels so only intended workflows target this runner.
- Do not expose runner to public/untrusted repos.

## Controls
- Keep runner registration token scoped and rotatable.
- Use pinned workflow actions/images where possible.
- Avoid secrets in PR-triggered untrusted contexts.
- Periodically rotate runner token and prune caches.

## Incident response
If compromise suspected:
1. Disable runner in Gitea UI.
2. Stop/remove runner container.
3. Rotate registration token.
4. Rotate any CI secrets potentially exposed.
5. Recreate runner with clean volumes.

## Rollback
- `docker compose stop gitea-runner`
- `docker compose rm -f gitea-runner`
- Optionally remove runner volumes (`gitea-runner-data`, cache volumes).
