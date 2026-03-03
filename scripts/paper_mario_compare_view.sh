#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_ROOT="${REALITYVK_PM_RUN_ROOT:-${ROOT_DIR}/build/parity-runs/paper-mario}"
CACHE_ROOT="${REALITYVK_PM_CACHE_ROOT:-${ROOT_DIR}/build/parity-cache/paper-mario}"
SCENARIO_ID="${REALITYVK_PM_SCENARIO_ID:-paper_mario_intro}"
VIEW_MODE="${REALITYVK_PM_COMPARE_VIEW_MODE:-side_by_side}"
REFRESH_REFERENCE="${REALITYVK_PM_REFRESH_REFERENCE:-0}"
VIEWER="${REALITYVK_PM_COMPARE_VIEWER:-eog}"
VISUAL_GATE="${REALITYVK_PM_COMPARE_VISUAL_GATE:-0}"
DUMPFB_FLIP_Y="${REALITYVK_PM_DUMPFB_FLIP_Y:-1}"
CLOSE_ALL_EOG="${REALITYVK_PM_COMPARE_CLOSE_ALL_EOG:-1}"

CAPTURE_METHOD_TAG="dumpfb_flip${DUMPFB_FLIP_Y}"

REFERENCE_CAPTURE="${CACHE_ROOT}/${SCENARIO_ID}.${CAPTURE_METHOD_TAG}.reference.ppm"
CANDIDATE_CAPTURE="${RUN_ROOT}/${SCENARIO_ID}.candidate.ppm"
METRICS_JSON="${RUN_ROOT}/${SCENARIO_ID}.metrics.json"
CAPTURE_CONTEXT_JSON="${RUN_ROOT}/${SCENARIO_ID}.capture-context.json"
REFERENCE_PNG="${RUN_ROOT}/${SCENARIO_ID}.reference.png"
CANDIDATE_PNG="${RUN_ROOT}/${SCENARIO_ID}.candidate.png"

COMPARE_OUT="${RUN_ROOT}/${SCENARIO_ID}.compare_${VIEW_MODE}.latest.png"
VIEWER_PID_FILE="${RUN_ROOT}/.paper_mario_compare_view.pid"

mkdir -p "${RUN_ROOT}" "${CACHE_ROOT}"

if [[ "${VIEW_MODE}" != "side_by_side" ]]; then
  echo "ERROR: REALITYVK_PM_COMPARE_VIEW_MODE must be 'side_by_side'." >&2
  exit 2
fi

if [[ "${REFRESH_REFERENCE}" != "0" && "${REFRESH_REFERENCE}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_REFRESH_REFERENCE must be 0 or 1." >&2
  exit 2
fi

if [[ "${VISUAL_GATE}" != "0" && "${VISUAL_GATE}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_COMPARE_VISUAL_GATE must be 0 or 1." >&2
  exit 2
fi

if [[ "${CLOSE_ALL_EOG}" != "0" && "${CLOSE_ALL_EOG}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_COMPARE_CLOSE_ALL_EOG must be 0 or 1." >&2
  exit 2
fi

echo "==> [compare-view] run parity capture"
REALITYVK_PM_REFRESH_REFERENCE="${REFRESH_REFERENCE}" \
REALITYVK_PM_VISUAL_GATE="${VISUAL_GATE}" \
  "${ROOT_DIR}/scripts/paper_mario_parity.sh"

REFERENCE_SOURCE="${REFERENCE_PNG}"
CANDIDATE_SOURCE="${CANDIDATE_PNG}"
if [[ ! -s "${REFERENCE_SOURCE}" ]]; then
  REFERENCE_SOURCE="${REFERENCE_CAPTURE}"
fi
if [[ ! -s "${CANDIDATE_SOURCE}" ]]; then
  CANDIDATE_SOURCE="${CANDIDATE_CAPTURE}"
fi

for required in "${REFERENCE_SOURCE}" "${CANDIDATE_SOURCE}"; do
  if [[ ! -s "${required}" ]]; then
    echo "ERROR: missing required compare artifact: ${required}" >&2
    exit 1
  fi
done

if command -v magick >/dev/null 2>&1; then
  IM_CONVERT=(magick)
elif command -v convert >/dev/null 2>&1; then
  IM_CONVERT=(convert)
else
  echo "ERROR: ImageMagick is required (magick/convert not found)." >&2
  exit 1
fi

echo "==> [compare-view] build side-by-side image (${VIEW_MODE})"
"${IM_CONVERT[@]}" \
  "${REFERENCE_SOURCE}" \
  "${CANDIDATE_SOURCE}" \
  +append \
  "${COMPARE_OUT}"

new_view="$(python3 - "${COMPARE_OUT}" <<'PY'
import sys
from pathlib import Path
print(Path(sys.argv[1]).resolve())
PY
)"

close_old_viewer() {
  if [[ -f "${VIEWER_PID_FILE}" ]]; then
    old_pid="$(cat "${VIEWER_PID_FILE}" 2>/dev/null || true)"
    if [[ -n "${old_pid}" ]] && kill -0 "${old_pid}" >/dev/null 2>&1; then
      kill "${old_pid}" >/dev/null 2>&1 || true
      sleep 0.15
    fi
    rm -f "${VIEWER_PID_FILE}"
  fi

  if [[ "${CLOSE_ALL_EOG}" == "1" ]]; then
    pkill -x eog >/dev/null 2>&1 || true
  fi
}

open_viewer() {
  if ! command -v eog >/dev/null 2>&1; then
    echo "ERROR: eog is required for compare view auto-open." >&2
    return
  fi
  VIEWER="eog"

  launch_with() {
    local _viewer="$1"
    case "${_viewer}" in
      eog)
        setsid eog --new-instance "${new_view}" >/dev/null 2>&1 < /dev/null &
        ;;
      *)
        return 1
        ;;
    esac
    launched_pid="$!"
    disown || true
    sleep 0.2
    if kill -0 "${launched_pid}" >/dev/null 2>&1; then
      echo "${launched_pid}" > "${VIEWER_PID_FILE}"
      return 0
    fi
    return 1
  }

  if launch_with "${VIEWER}"; then
    return
  fi

  rm -f "${VIEWER_PID_FILE}"
  echo "WARN: could not keep a viewer process alive; image is at ${new_view}" >&2
}

echo "==> [compare-view] close previous image and open new comparison"
close_old_viewer
open_viewer

echo "compare image: ${new_view}"
echo "reference: ${REFERENCE_SOURCE}"
echo "candidate: ${CANDIDATE_SOURCE}"
echo "metrics: ${METRICS_JSON}"
if [[ -s "${CAPTURE_CONTEXT_JSON}" ]]; then
  echo "capture context: ${CAPTURE_CONTEXT_JSON}"
fi
