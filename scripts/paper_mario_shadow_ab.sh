#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT_DIR}/scripts/lib/validation.sh"
source "${ROOT_DIR}/scripts/lib/compare_view.sh"

SCENARIO_ID="${REALITYVK_PM_SCENARIO_ID:-paper_mario_intro}"
FRAMES_OVERRIDE="${REALITYVK_PM_FRAMES_OVERRIDE:-20}"
PROFILE="${REALITYVK_PM_PROFILE:-basic}"
RUN_ROOT_BASE="${REALITYVK_PM_SHADOW_AB_RUN_ROOT:-${ROOT_DIR}/build/parity-runs/paper-mario/shadow-ab}"
CLOSE_ALL_EOG="${REALITYVK_PM_AUTO_COMPARE_CLOSE_ALL_EOG:-1}"
AUTO_OPEN="${REALITYVK_PM_SHADOW_AB_AUTO_OPEN:-1}"

usage() {
  cat <<'EOF_USAGE'
Usage:
  paper_mario_shadow_ab.sh [options]

Options:
  --scenario-id <id>      Scenario id (default: paper_mario_intro)
  --frames <n>            Frame override (default: 20)
  --profile <basic|deep>  Parity profile for both runs (default: basic)
  --run-root <dir>        Shadow A/B run root (default: build/parity-runs/paper-mario/shadow-ab)
  --auto-open <0|1>       Open off-vs-on image in eog (default: 1)
  --close-all-eog <0|1>   Close all eog windows before open (default: 1)
  -h, --help              Show this help
EOF_USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scenario-id)
      SCENARIO_ID="$2"
      shift 2
      ;;
    --frames)
      FRAMES_OVERRIDE="$2"
      shift 2
      ;;
    --profile)
      PROFILE="$2"
      shift 2
      ;;
    --run-root)
      RUN_ROOT_BASE="$2"
      shift 2
      ;;
    --auto-open)
      AUTO_OPEN="$2"
      shift 2
      ;;
    --close-all-eog)
      CLOSE_ALL_EOG="$2"
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

rvk2_require_enum "--profile" "${PROFILE}" "basic" "deep"
rvk2_require_uint_ge "--frames" "${FRAMES_OVERRIDE}" 0
rvk2_require_bool "--auto-open" "${AUTO_OPEN}"
rvk2_require_bool "--close-all-eog" "${CLOSE_ALL_EOG}"

stamp="$(date -u +%Y%m%d-%H%M%SZ)"
run_root="${RUN_ROOT_BASE}/${stamp}"
off_root="${run_root}/off"
on_root="${run_root}/on"
mkdir -p "${off_root}" "${on_root}"

run_shadow_case() {
  local label="$1"
  local shadow_draw="$2"
  local shadow_present="$3"
  local run_root_case="$4"
  echo "==> [shadow-ab] ${label}: shadow_draw=${shadow_draw} shadow_present=${shadow_present} profile=${PROFILE} frames=${FRAMES_OVERRIDE}"
  env \
    REALITYVK_PM_SCENARIO_ID="${SCENARIO_ID}" \
    REALITYVK_PM_PROFILE="${PROFILE}" \
    REALITYVK_PM_VISUAL_GATE=0 \
    REALITYVK_PM_AUTO_COMPARE_VIEW=0 \
    REALITYVK_PM_KNOB_TRACK_ENABLE=0 \
    REALITYVK_PM_FRAMES_OVERRIDE="${FRAMES_OVERRIDE}" \
    REALITYVK_PM_RUN_ROOT="${run_root_case}" \
    REALITYVK_RVK2_SHADOW_DRAW="${shadow_draw}" \
    REALITYVK_RVK2_SHADOW_PRESENT="${shadow_present}" \
    "${ROOT_DIR}/scripts/paper_mario_parity.sh"
}

run_shadow_case "off" "0" "0" "${off_root}"
run_shadow_case "on" "1" "1" "${on_root}"

off_png="${off_root}/${SCENARIO_ID}.candidate.png"
on_png="${on_root}/${SCENARIO_ID}.candidate.png"
off_ppm="${off_root}/${SCENARIO_ID}.candidate.ppm"
on_ppm="${on_root}/${SCENARIO_ID}.candidate.ppm"

if [[ ! -s "${off_png}" ]]; then
  off_png="${off_ppm}"
fi
if [[ ! -s "${on_png}" ]]; then
  on_png="${on_ppm}"
fi

if [[ ! -s "${off_png}" ]]; then
  echo "ERROR: shadow-off candidate artifact missing: ${off_png}" >&2
  exit 1
fi
if [[ ! -s "${on_png}" ]]; then
  echo "ERROR: shadow-on candidate artifact missing: ${on_png}" >&2
  exit 1
fi

compare_out="${run_root}/${SCENARIO_ID}.shadow_off_vs_on.latest.png"
viewer_pid_file="${RUN_ROOT_BASE}/.shadow_ab_compare_view.pid"
rvk2_build_side_by_side_image "${off_png}" "${on_png}" "${compare_out}"

if [[ "${AUTO_OPEN}" == "1" ]]; then
  rvk2_close_eog_view "${viewer_pid_file}" "${CLOSE_ALL_EOG}"
  rvk2_open_eog_view "${compare_out}" "${viewer_pid_file}"
fi

echo "shadow-ab run root: ${run_root}"
echo "shadow off candidate: ${off_png}"
echo "shadow on candidate: ${on_png}"
echo "shadow off vs on image: ${compare_out}"
