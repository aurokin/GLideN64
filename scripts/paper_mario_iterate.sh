#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT_DIR}/scripts/lib/validation.sh"

PRESET="${REALITYVK_PM_ITERATE_PRESET:-fast}"
RUNS="${REALITYVK_PM_ITERATE_RUNS:-1}"
SLEEP_SECONDS="${REALITYVK_PM_ITERATE_SLEEP_SECONDS:-0}"
SCENARIO_ID="${REALITYVK_PM_SCENARIO_ID:-paper_mario_intro}"
FRAMES_OVERRIDE="${REALITYVK_PM_FRAMES_OVERRIDE:-}"
AUTO_COMPARE_VIEW="${REALITYVK_PM_AUTO_COMPARE_VIEW:-1}"

FAST_FRAMES_DEFAULT="${REALITYVK_PM_ITERATE_FAST_FRAMES:-20}"
CHECKPOINT_FRAMES_DEFAULT="${REALITYVK_PM_ITERATE_CHECKPOINT_FRAMES:-120}"

usage() {
  cat <<'EOF_USAGE'
Usage:
  paper_mario_iterate.sh [options]

Options:
  --preset <fast|checkpoint>   Preset profile (default: fast)
  --runs <n>                   Number of runs (default: 1, 0=infinite)
  --sleep-seconds <n>          Sleep between runs (default: 0)
  --frames <n>                 Override frame count for parity run
  --scenario-id <id>           Scenario id (default: paper_mario_intro)
  --auto-view <0|1>            Open compare viewer after each run (default: 1)
  -h, --help                   Show this help
EOF_USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --preset)
      PRESET="$2"
      shift 2
      ;;
    --runs)
      RUNS="$2"
      shift 2
      ;;
    --sleep-seconds)
      SLEEP_SECONDS="$2"
      shift 2
      ;;
    --frames)
      FRAMES_OVERRIDE="$2"
      shift 2
      ;;
    --scenario-id)
      SCENARIO_ID="$2"
      shift 2
      ;;
    --auto-view)
      AUTO_COMPARE_VIEW="$2"
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

rvk2_require_enum "--preset" "${PRESET}" "fast" "checkpoint"
rvk2_require_uint_ge "--runs" "${RUNS}" 0
rvk2_require_uint_ge "--sleep-seconds" "${SLEEP_SECONDS}" 0
rvk2_require_bool "--auto-view" "${AUTO_COMPARE_VIEW}"
if [[ -n "${FRAMES_OVERRIDE}" ]]; then
  rvk2_require_uint_ge "--frames" "${FRAMES_OVERRIDE}" 0
fi

profile="basic"
deep_telemetry="0"
deep_archive="0"
deep_replay_stateful="0"
default_frames="${FAST_FRAMES_DEFAULT}"
case "${PRESET}" in
  fast)
    ;;
  checkpoint)
    profile="deep"
    deep_telemetry="1"
    deep_archive="1"
    deep_replay_stateful="1"
    default_frames="${CHECKPOINT_FRAMES_DEFAULT}"
    ;;
esac

if [[ -z "${FRAMES_OVERRIDE}" ]]; then
  FRAMES_OVERRIDE="${default_frames}"
fi
rvk2_require_uint_ge "preset frames" "${FRAMES_OVERRIDE}" 0

run_once() {
  local run_index="$1"
  echo "==> [iterate] run=${run_index} preset=${PRESET} scenario=${SCENARIO_ID} frames=${FRAMES_OVERRIDE}"
  env \
    REALITYVK_PM_SCENARIO_ID="${SCENARIO_ID}" \
    REALITYVK_PM_PROFILE="${profile}" \
    REALITYVK_PM_DEEP_TELEMETRY="${deep_telemetry}" \
    REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE="${deep_archive}" \
    REALITYVK_PM_DEEP_TELEMETRY_REPLAY_STATEFUL="${deep_replay_stateful}" \
    REALITYVK_PM_VISUAL_GATE=0 \
    REALITYVK_PM_AUTO_COMPARE_VIEW="${AUTO_COMPARE_VIEW}" \
    REALITYVK_PM_FRAMES_OVERRIDE="${FRAMES_OVERRIDE}" \
    "${ROOT_DIR}/scripts/paper_mario_parity.sh"
}

if [[ "${RUNS}" == "0" ]]; then
  idx=1
  while true; do
    run_once "${idx}"
    idx=$((idx + 1))
    if [[ "${SLEEP_SECONDS}" != "0" ]]; then
      sleep "${SLEEP_SECONDS}"
    fi
  done
else
  idx=1
  while (( idx <= RUNS )); do
    run_once "${idx}"
    if (( idx < RUNS )) && [[ "${SLEEP_SECONDS}" != "0" ]]; then
      sleep "${SLEEP_SECONDS}"
    fi
    idx=$((idx + 1))
  done
fi
