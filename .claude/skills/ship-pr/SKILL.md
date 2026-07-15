---
name: ship-pr
description: Use when pushing a Bramble branch, opening a pull request, waiting on CI, or merging. Covers the Gitea API (gh does NOT work here), the CI checks that fail for non-obvious reasons, and the merge/branch-deletion order that avoids auto-closing a PR unmerged.
---

# Shipping a branch on Bramble (Gitea, not GitHub)

`gh` does not speak Gitea. Use curl against the API. Canonical remote is Gitea;
GitHub is a mirror with no auto-sync.

- API base: `https://git.idiotica.org/api/v1`, repo `dumbot/bramble`, base branch `main`.
- PAT: `~/src/bramble-meta/secrets/gitea-pat` (the file is just the token).
- Use **curl**, not python-requests: the workstation trusts the internal CA, but
  python's bundle does not (you will get CERTIFICATE_VERIFY_FAILED).
- Runbook with more endpoints: `~/src/bramble-meta/runbooks/gitea-api.md`.

## Before you push

- **Branch name must match `fix/** feat/** feature/** chore/** ci/**` or CI NEVER
  RUNS** (and the PR sits green-less forever).
- **No em dashes** anywhere and **no AI attribution** (no `Co-Authored-By`, no
  session links) in commits or the PR body. Check the DIFF, not whole files:
  `git diff main..HEAD | grep -P '^\+' | grep -cP '\x{2014}'` must be 0.
  (Pre-existing em dashes elsewhere in a file are fine; CI only sees your lines.)

## Open the PR

```sh
PAT=$(cat ~/src/bramble-meta/secrets/gitea-pat)
git push -u origin <branch>
# build the JSON with python json.dumps (safe escaping), POST it with curl:
curl -sS -X POST -H "Authorization: token $PAT" -H "Content-Type: application/json" \
  "https://git.idiotica.org/api/v1/repos/dumbot/bramble/pulls" -d @pr.json
# body: {"title": "...", "head": "<branch>", "base": "main", "body": "..."}
```

## Wait for CI, then merge

Poll the combined status (this is what branch protection evaluates):
```sh
curl -sS -H "Authorization: token $PAT" \
  "https://git.idiotica.org/api/v1/repos/dumbot/bramble/commits/<sha>/status"
# -> .state = pending | success | failure ; .statuses[] has each check + target_url
```
**GATE THE MERGE ON THE POLLED STATE.** Never chain `<poll output> && merge` in
one command: a watcher that prints `failure` still exits 0, and the merge fires
anyway (this landed a red PR once; the red was an infra flake, but only luck made
it benign). Branch protection does NOT backstop you: `enable_status_check` is
false on this repo, so Gitea will happily merge a red PR. Read the state, then
merge in a separate command only if it is `success`.

Merge (main's history is squash-merged, one commit per PR titled `... (#N)`):
```sh
curl -sS -X POST -H "Authorization: token $PAT" -H "Content-Type: application/json" \
  ".../pulls/<N>/merge" -d '{"Do":"squash","MergeTitleField":"<title> (#N)"}'
```
**Then VERIFY `merged: true` before deleting the branch.** Deleting first
auto-closes the PR UNMERGED (Gitea re-checks mergeability when the base moves).
Gitea usually auto-deletes the head branch on merge anyway.

## CI checks that fail for non-obvious reasons

- **RPC contract**: `api/openapi.yaml` must list EXACTLY the methods
  `main/rpc_methods.c` registers. Add a `/rpc/bramble.yourMethod:` path entry for
  every new `rpc_register(...)` or CI hard-fails. Reproduce locally:
  `bash scripts/check-rpc-contract.sh`.
- **cppcheck**: `--error-exitcode=2`, so even a false positive is red. A classic:
  `uninitvar` on a buffer only read behind a guard (`have_x ? buf : NULL`).
  Zero-init it rather than fighting the analyzer.
- **clang-format**: must be the CI runner's v14, not your local binary:
  `docker run --rm -v "$PWD:/w" -w /w bramble/runner-full:22.04-go126 clang-format -i <files>`
- **Emulator scenario suite**: firmware nodes run in REAL time, so timing-dependent
  scenarios flake. `emu-dm-desync` reproduces its symptom only ~2/3 of runs and is
  retried up to 6x in `emulator/ci/run_scenarios.sh`. If you add a scenario that
  depends on a race, measure its reproduction rate and retry it; do not just
  re-run CI and hope.

## If CI goes red

Pull the failing job's log and diagnose before acting:
```sh
curl -sS -H "Authorization: token $PAT" ".../actions/jobs/<job_id>/logs"
```
Flaky-shaped (timeout, runner died, a known real-time scenario) can be re-run.
Anything else gets a reproduction and a real fix.
