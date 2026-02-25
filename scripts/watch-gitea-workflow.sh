#!/usr/bin/env bash
set -euo pipefail

OWNER="justinlindh"
REPO="bramble"
WORKFLOW="webapp-build-publish.yml"
COMMIT_PREFIX=""
WAIT=0
INTERVAL=5
TIMEOUT=900
SHOW_LOG_ON_FAIL=1

usage() {
  cat <<USAGE
Usage: watch-gitea-workflow.sh [options]

Options:
  --owner <owner>         Repo owner (default: justinlindh)
  --repo <repo>           Repo name (default: bramble)
  --workflow <file>       Workflow file id (default: webapp-build-publish.yml)
  --commit <sha-prefix>   Filter to latest run with this commit prefix
  --wait                  Poll until run finishes
  --interval <sec>        Poll interval when --wait is set (default: 5)
  --timeout <sec>         Max wait time (default: 900)
  --no-fail-log           Do not print failing job log tail
  -h, --help              Show help
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --owner) OWNER="$2"; shift 2 ;;
    --repo) REPO="$2"; shift 2 ;;
    --workflow) WORKFLOW="$2"; shift 2 ;;
    --commit) COMMIT_PREFIX="$2"; shift 2 ;;
    --wait) WAIT=1; shift ;;
    --interval) INTERVAL="$2"; shift 2 ;;
    --timeout) TIMEOUT="$2"; shift 2 ;;
    --no-fail-log) SHOW_LOG_ON_FAIL=0; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown arg: $1" >&2; usage >&2; exit 2 ;;
  esac
done

status_name() {
  case "$1" in
    1) echo "success" ;;
    2) echo "failure" ;;
    3) echo "cancelled" ;;
    4) echo "running" ;;
    5) echo "skipped" ;;
    *) echo "status_$1" ;;
  esac
}

psql_tsv() {
  local q="$1"
  docker exec gitea-db psql -U gitea -d gitea -At -F $'\t' -c "$q"
}

latest_run_query() {
  local commit_filter=""
  if [[ -n "$COMMIT_PREFIX" ]]; then
    commit_filter="AND ar.commit_sha LIKE '${COMMIT_PREFIX}%'"
  fi
  cat <<SQL
SELECT ar.id, ar.index, ar.commit_sha, ar.status, ar.event,
       COALESCE(ar.started,0), COALESCE(ar.stopped,0),
       to_char(to_timestamp(ar.created), 'YYYY-MM-DD HH24:MI:SS')
FROM action_run ar
JOIN repository r ON r.id = ar.repo_id
WHERE r.owner_name='${OWNER}'
  AND r.lower_name='${REPO}'
  AND ar.workflow_id='${WORKFLOW}'
  ${commit_filter}
ORDER BY ar.id DESC
LIMIT 1;
SQL
}

job_rows_query() {
  local run_id="$1"
  cat <<SQL
SELECT arj.id, arj.name, arj.status,
       COALESCE(arj.started,0), COALESCE(arj.stopped,0),
       COALESCE(at.log_filename,''), COALESCE(at.id,0)
FROM action_run_job arj
LEFT JOIN action_task at ON at.id = arj.task_id
WHERE arj.run_id=${run_id}
ORDER BY arj.id;
SQL
}

print_run() {
  local row="$1"
  IFS=$'\t' read -r run_id run_index commit_sha run_status run_event started stopped created_at <<< "$row"
  echo "run_id=${run_id} run_index=${run_index} workflow=${WORKFLOW} event=${run_event}"
  echo "commit=${commit_sha} created=${created_at} started=${started} stopped=${stopped} status=$(status_name "$run_status")(${run_status})"

  while IFS=$'\t' read -r job_id job_name job_status job_started job_stopped log_filename task_id; do
    [[ -z "${job_id}" ]] && continue
    echo "job_id=${job_id} name=${job_name} status=$(status_name "$job_status")(${job_status}) started=${job_started} stopped=${job_stopped} task_id=${task_id}"
    if [[ "$SHOW_LOG_ON_FAIL" -eq 1 && "$job_status" == "2" && -n "$log_filename" ]]; then
      local path="/home/user/src/dockers/gitea/data/gitea/actions_log/${log_filename}"
      if [[ -f "$path" ]]; then
        echo "--- failing job log tail (${path}) ---"
        zstdcat "$path" | tail -n 80 || true
        echo "--- end failing job log tail ---"
      fi
    fi
  done < <(psql_tsv "$(job_rows_query "$run_id")")

  # Some terminal outcomes (e.g. skipped) may not populate stopped timestamp.
  if [[ "$run_status" == "1" || "$run_status" == "2" || "$run_status" == "3" || "$run_status" == "5" ]]; then
    return 0
  fi
  if [[ "$stopped" == "0" ]]; then
    return 10
  fi
  return 0
}

start_ts=$(date +%s)
while true; do
  row="$(psql_tsv "$(latest_run_query)")"
  if [[ -z "$row" ]]; then
    echo "no run found for ${OWNER}/${REPO} workflow=${WORKFLOW} commit_prefix=${COMMIT_PREFIX:-<none>}" >&2
    exit 1
  fi

  print_run "$row" || run_active=$?
  if [[ "${run_active:-0}" != "10" ]]; then
    break
  fi

  if [[ "$WAIT" -ne 1 ]]; then
    break
  fi

  now=$(date +%s)
  if (( now - start_ts > TIMEOUT )); then
    echo "timeout waiting for workflow completion (${TIMEOUT}s)" >&2
    exit 124
  fi
  sleep "$INTERVAL"
  echo "--- polling ---"
done
