#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT_DIR}/scripts/lib/validation.sh"
source "${ROOT_DIR}/scripts/lib/compare_view.sh"
RUN_ROOT="${REALITYVK_PM_RUN_ROOT:-${ROOT_DIR}/build/parity-runs/paper-mario}"
CACHE_ROOT="${REALITYVK_PM_CACHE_ROOT:-${ROOT_DIR}/build/parity-cache/paper-mario}"
SCENARIO_ID="${REALITYVK_PM_SCENARIO_ID:-paper_mario_intro}"
REFRESH_REFERENCE="${REALITYVK_PM_REFRESH_REFERENCE:-0}"
VISUAL_GATE="${REALITYVK_PM_VISUAL_GATE:-${REALITYVK_PM_COMPARE_VISUAL_GATE:-0}}"
DUMPFB_FLIP_Y="${REALITYVK_PM_DUMPFB_FLIP_Y:-1}"
CAPTURE_SCALE_DIV="${REALITYVK_PM_CAPTURE_SCALE_DIV:-1}"
CLOSE_ALL_EOG="${REALITYVK_PM_AUTO_COMPARE_CLOSE_ALL_EOG:-${REALITYVK_PM_COMPARE_CLOSE_ALL_EOG:-1}}"

CAPTURE_METHOD_TAG="dumpfb_scale${CAPTURE_SCALE_DIV}_flip${DUMPFB_FLIP_Y}"

REFERENCE_CAPTURE="${CACHE_ROOT}/${SCENARIO_ID}.${CAPTURE_METHOD_TAG}.reference.ppm"
CANDIDATE_CAPTURE="${RUN_ROOT}/${SCENARIO_ID}.candidate.ppm"
METRICS_JSON="${RUN_ROOT}/${SCENARIO_ID}.metrics.json"
CAPTURE_CONTEXT_JSON="${RUN_ROOT}/${SCENARIO_ID}.capture-context.json"
REFERENCE_PNG="${RUN_ROOT}/${SCENARIO_ID}.reference.png"
CANDIDATE_PNG="${RUN_ROOT}/${SCENARIO_ID}.candidate.png"

COMPARE_OUT="${RUN_ROOT}/${SCENARIO_ID}.compare_side_by_side.latest.png"
VIEWER_PID_FILE="${RUN_ROOT}/.paper_mario_compare_view.pid"

mkdir -p "${RUN_ROOT}" "${CACHE_ROOT}"

rvk2_require_bool "REALITYVK_PM_REFRESH_REFERENCE" "${REFRESH_REFERENCE}"
rvk2_require_bool "REALITYVK_PM_VISUAL_GATE (or legacy REALITYVK_PM_COMPARE_VISUAL_GATE)" "${VISUAL_GATE}"
rvk2_require_bool "REALITYVK_PM_AUTO_COMPARE_CLOSE_ALL_EOG (or legacy REALITYVK_PM_COMPARE_CLOSE_ALL_EOG)" "${CLOSE_ALL_EOG}"
rvk2_require_uint_ge "REALITYVK_PM_CAPTURE_SCALE_DIV" "${CAPTURE_SCALE_DIV}" 1

echo "==> [compare-view] run parity capture"
REALITYVK_PM_REFRESH_REFERENCE="${REFRESH_REFERENCE}" \
REALITYVK_PM_VISUAL_GATE="${VISUAL_GATE}" \
REALITYVK_PM_AUTO_COMPARE_VIEW=0 \
  "${ROOT_DIR}/scripts/paper_mario_parity.sh"

REFERENCE_SOURCE="${REFERENCE_PNG}"
CANDIDATE_SOURCE="${CANDIDATE_PNG}"
if [[ ! -s "${REFERENCE_SOURCE}" ]]; then
  REFERENCE_SOURCE="${REFERENCE_CAPTURE}"
fi
if [[ ! -s "${CANDIDATE_SOURCE}" ]]; then
  CANDIDATE_SOURCE="${CANDIDATE_CAPTURE}"
fi
if [[ -s "${CAPTURE_CONTEXT_JSON}" ]]; then
  mapfile -t context_paths < <(python3 - "${CAPTURE_CONTEXT_JSON}" <<'PY'
import json
import sys
from pathlib import Path

context_path = Path(sys.argv[1])
try:
    payload = json.loads(context_path.read_text(encoding="utf-8"))
except Exception:
    print("")
    print("")
    raise SystemExit(0)

reference = payload.get("reference", {})
candidate = payload.get("candidate", {})
print(str(reference.get("capture_path", "")))
print(str(candidate.get("capture_path", "")))
PY
)
  if [[ -n "${context_paths[0]:-}" && -s "${context_paths[0]}" ]]; then
    REFERENCE_SOURCE="${context_paths[0]}"
  fi
  if [[ -n "${context_paths[1]:-}" && -s "${context_paths[1]}" ]]; then
    CANDIDATE_SOURCE="${context_paths[1]}"
  fi
fi

for required in "${REFERENCE_SOURCE}" "${CANDIDATE_SOURCE}"; do
  if [[ ! -s "${required}" ]]; then
    echo "ERROR: missing required compare artifact: ${required}" >&2
    exit 1
  fi
done

echo "==> [compare-view] build side-by-side image"
rvk2_build_side_by_side_image "${REFERENCE_SOURCE}" "${CANDIDATE_SOURCE}" "${COMPARE_OUT}"

new_view="$(python3 - "${COMPARE_OUT}" <<'PY'
import sys
from pathlib import Path
print(Path(sys.argv[1]).resolve())
PY
)"

echo "==> [compare-view] close previous image and open new comparison"
rvk2_close_eog_view "${VIEWER_PID_FILE}" "${CLOSE_ALL_EOG}"
rvk2_open_eog_view "${new_view}" "${VIEWER_PID_FILE}"

echo "compare image: ${new_view}"
echo "reference: ${REFERENCE_SOURCE}"
echo "candidate: ${CANDIDATE_SOURCE}"
echo "metrics: ${METRICS_JSON}"
if [[ -s "${CAPTURE_CONTEXT_JSON}" ]]; then
  echo "capture context: ${CAPTURE_CONTEXT_JSON}"
fi
