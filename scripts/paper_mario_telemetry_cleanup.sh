#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_ROOT="${REALITYVK_PM_RUN_ROOT:-${ROOT_DIR}/build/parity-runs/paper-mario}"
SCENARIO_ID="${REALITYVK_PM_SCENARIO_ID:-paper_mario_intro}"
DRY_RUN="${REALITYVK_PM_TELEMETRY_PRUNE_DRY_RUN:-0}"
TELEMETRY_ROOT="${REALITYVK_PM_TELEMETRY_ROOT:-${RUN_ROOT}/telemetry}"

usage() {
  cat <<'EOF_USAGE'
Usage:
  paper_mario_telemetry_cleanup.sh [options]

Options:
  --run-root <dir>       Parity run root (default: build/parity-runs/paper-mario)
  --telemetry-root <dir> Telemetry directory (default: <run-root>/telemetry)
  --scenario-id <id>     Scenario prefix to prune (default: paper_mario_intro)
  --dry-run <0|1>        Print removals without deleting (default: 0)
  -h, --help             Show this help
EOF_USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --run-root)
      RUN_ROOT="$2"
      shift 2
      ;;
    --telemetry-root)
      TELEMETRY_ROOT="$2"
      shift 2
      ;;
    --scenario-id)
      SCENARIO_ID="$2"
      shift 2
      ;;
    --dry-run)
      DRY_RUN="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: unknown option '$1'" >&2
      usage
      exit 2
      ;;
  esac
done

if [[ "${DRY_RUN}" != "0" && "${DRY_RUN}" != "1" ]]; then
  echo "ERROR: --dry-run must be 0 or 1." >&2
  exit 2
fi

if [[ ! -d "${TELEMETRY_ROOT}" ]]; then
  echo "telemetry cleanup: nothing to do (missing directory: ${TELEMETRY_ROOT})"
  exit 0
fi

keep_entry() {
  local base="$1"
  case "${base}" in
    "${SCENARIO_ID}.candidate.trace.tsv"|\
    "${SCENARIO_ID}.candidate.packet.tsv"|\
    "${SCENARIO_ID}.candidate.packet.replay.json"|\
    "${SCENARIO_ID}.candidate.frame-forensics.tsv"|\
    "${SCENARIO_ID}.candidate.frame-forensics.summary.txt"|\
    "${SCENARIO_ID}.candidate.frame-forensics.active.summary.txt"|\
    "${SCENARIO_ID}.candidate.depth-blit-summary.json"|\
    "${SCENARIO_ID}.candidate.command-census.json"|\
    "${SCENARIO_ID}.candidate.command-census.md"|\
    "${SCENARIO_ID}.candidate.missing-region-focus.json"|\
    "${SCENARIO_ID}.candidate.history-merge.tsv"|\
    "${SCENARIO_ID}.candidate.overwrite.tsv"|\
    "${SCENARIO_ID}.candidate.triangle-packet.tsv"|\
    "${SCENARIO_ID}.candidate.executor-present.ppm"|\
    "${SCENARIO_ID}.candidate.launch.log"|\
    "${SCENARIO_ID}.telemetry.bundle.json"|\
    "${SCENARIO_ID}.deviation"|\
    "${SCENARIO_ID}.extra_non_black")
      return 0
      ;;
    *)
      return 1
      ;;
  esac
}

mapfile -t entries < <(find "${TELEMETRY_ROOT}" -mindepth 1 -maxdepth 1 \( -type f -o -type d \) -name "${SCENARIO_ID}.*" | sort)

removed=0
kept=0
for entry in "${entries[@]}"; do
  base="$(basename "${entry}")"
  if keep_entry "${base}"; then
    kept=$((kept + 1))
    continue
  fi
  if [[ "${DRY_RUN}" == "1" ]]; then
    echo "telemetry cleanup [dry-run] remove: ${entry}"
  else
    rm -rf "${entry}"
    echo "telemetry cleanup remove: ${entry}"
  fi
  removed=$((removed + 1))
done

echo "telemetry cleanup summary: root=${TELEMETRY_ROOT} scenario=${SCENARIO_ID} removed=${removed} kept=${kept} dry_run=${DRY_RUN}"
