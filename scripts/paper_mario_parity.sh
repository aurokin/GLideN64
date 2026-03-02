#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="${REALITYVK_PM_MANIFEST:-${ROOT_DIR}/tests/smoke/scenarios.tsv}"
SCENARIO_ID="${REALITYVK_PM_SCENARIO_ID:-paper_mario_intro}"

CANDIDATE_PLUGIN="${REALITYVK_PM_CANDIDATE_PLUGIN:-${ROOT_DIR}/build/release-vulkan-smoke/plugin/Release/mupen64plus-video-RealityVK.so}"
CANDIDATE_CORELIB="${REALITYVK_PM_CANDIDATE_CORELIB:-/home/auro/code/mupen/mupen64plus-core/projects/unix/libmupen64plus.so.2}"
REFERENCE_PLUGIN="${REALITYVK_PM_REFERENCE_PLUGIN:-/home/auro/code/gliden64-upstream/build-release/plugin/Release/mupen64plus-video-GLideN64.so}"
REFERENCE_CORELIB="${REALITYVK_PM_REFERENCE_CORELIB:-/home/auro/code/mupen/mupen64plus-core-upstream/projects/unix/libmupen64plus.so.2}"

CACHE_ROOT="${REALITYVK_PM_CACHE_ROOT:-${ROOT_DIR}/build/parity-cache/paper-mario}"
RUN_ROOT="${REALITYVK_PM_RUN_ROOT:-${ROOT_DIR}/build/parity-runs/paper-mario}"

REFRESH_REFERENCE="${REALITYVK_PM_REFRESH_REFERENCE:-0}"
VISUAL_GATE="${REALITYVK_PM_VISUAL_GATE:-1}"
RMSE_MAX="${REALITYVK_PM_VISUAL_RMSE_MAX:-0.25}"
MAE_MAX="${REALITYVK_PM_VISUAL_MAE_MAX:-}"
CAPTURE_DEPTH_SUMMARY="${REALITYVK_PM_CAPTURE_DEPTH_SUMMARY:-1}"
REQUIRE_NO_DEPTH_BLIT_FAIL="${REALITYVK_PM_REQUIRE_NO_DEPTH_BLIT_FAIL:-0}"
REQUIRE_DEPTH_BLIT_STATS="${REALITYVK_PM_REQUIRE_DEPTH_BLIT_STATS:-0}"
REQUIRE_NON_BLACK_CAPTURE="${REALITYVK_PM_REQUIRE_NON_BLACK_CAPTURE:-1}"
CAPTURE_RETRY_COUNT="${REALITYVK_PM_CAPTURE_RETRY_COUNT:-6}"
CAPTURE_RETRY_STEP_FRAMES="${REALITYVK_PM_CAPTURE_RETRY_STEP_FRAMES:-20}"
CAPTURE_RETRY_RESUME_MS="${REALITYVK_PM_CAPTURE_RETRY_RESUME_MS:-250}"
CAPTURE_MIN_NONBLACK_RATIO="${REALITYVK_PM_CAPTURE_MIN_NONBLACK_RATIO:-0.001}"
CAPTURE_MIN_MEAN_LUMA="${REALITYVK_PM_CAPTURE_MIN_MEAN_LUMA:-0.002}"
# Paper Mario parity defaults to explicit agent-side flip so captures match live window orientation.
# Override with REALITYVK_PM_DUMPFB_FLIP_Y=0 when raw dump orientation is needed.
DUMPFB_FLIP_Y="${REALITYVK_PM_DUMPFB_FLIP_Y:-1}"
LAUNCH_WITH_PTY="${REALITYVK_PM_LAUNCH_WITH_PTY:-1}"

if [[ ! -f "${MANIFEST}" ]]; then
  echo "ERROR: scenario manifest not found: ${MANIFEST}" >&2
  exit 2
fi

if [[ "${REFRESH_REFERENCE}" != "0" && "${REFRESH_REFERENCE}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_REFRESH_REFERENCE must be 0 or 1." >&2
  exit 2
fi

if [[ "${VISUAL_GATE}" != "0" && "${VISUAL_GATE}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_VISUAL_GATE must be 0 or 1." >&2
  exit 2
fi

if [[ "${CAPTURE_DEPTH_SUMMARY}" != "0" && "${CAPTURE_DEPTH_SUMMARY}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_CAPTURE_DEPTH_SUMMARY must be 0 or 1." >&2
  exit 2
fi

if [[ "${REQUIRE_NO_DEPTH_BLIT_FAIL}" != "0" && "${REQUIRE_NO_DEPTH_BLIT_FAIL}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_REQUIRE_NO_DEPTH_BLIT_FAIL must be 0 or 1." >&2
  exit 2
fi

if [[ "${REQUIRE_DEPTH_BLIT_STATS}" != "0" && "${REQUIRE_DEPTH_BLIT_STATS}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_REQUIRE_DEPTH_BLIT_STATS must be 0 or 1." >&2
  exit 2
fi

if [[ "${REQUIRE_NON_BLACK_CAPTURE}" != "0" && "${REQUIRE_NON_BLACK_CAPTURE}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_REQUIRE_NON_BLACK_CAPTURE must be 0 or 1." >&2
  exit 2
fi

if ! [[ "${CAPTURE_RETRY_COUNT}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_CAPTURE_RETRY_COUNT must be an integer >= 0." >&2
  exit 2
fi

if ! [[ "${CAPTURE_RETRY_STEP_FRAMES}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_CAPTURE_RETRY_STEP_FRAMES must be an integer >= 0." >&2
  exit 2
fi

if ! [[ "${CAPTURE_RETRY_RESUME_MS}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_CAPTURE_RETRY_RESUME_MS must be an integer >= 0." >&2
  exit 2
fi

if ! [[ "${CAPTURE_MIN_NONBLACK_RATIO}" =~ ^[0-9]*\.?[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_CAPTURE_MIN_NONBLACK_RATIO must be numeric." >&2
  exit 2
fi

if ! [[ "${CAPTURE_MIN_MEAN_LUMA}" =~ ^[0-9]*\.?[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_CAPTURE_MIN_MEAN_LUMA must be numeric." >&2
  exit 2
fi

if [[ "${DUMPFB_FLIP_Y}" != "0" && "${DUMPFB_FLIP_Y}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_DUMPFB_FLIP_Y must be 0 or 1." >&2
  exit 2
fi

if [[ "${LAUNCH_WITH_PTY}" != "0" && "${LAUNCH_WITH_PTY}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_LAUNCH_WITH_PTY must be 0 or 1." >&2
  exit 2
fi

if [[ ! -f "${CANDIDATE_PLUGIN}" ]]; then
  echo "ERROR: candidate plugin not found: ${CANDIDATE_PLUGIN}" >&2
  exit 2
fi

if [[ ! -f "${REFERENCE_CORELIB}" ]]; then
  echo "ERROR: reference core library not found: ${REFERENCE_CORELIB}" >&2
  exit 2
fi

if [[ ! -f "${CANDIDATE_CORELIB}" ]]; then
  echo "ERROR: candidate core library not found: ${CANDIDATE_CORELIB}" >&2
  exit 2
fi

scenario_line="$(
  awk -F '\t' -v id="${SCENARIO_ID}" '
    $0 ~ /^[[:space:]]*#/ { next }
    $1 == id { print; exit }
  ' "${MANIFEST}"
)"

if [[ -z "${scenario_line}" ]]; then
  echo "ERROR: scenario '${SCENARIO_ID}' not found in ${MANIFEST}" >&2
  exit 2
fi

IFS=$'\t' read -r _scenario ROM_PATH FRAMES SCENARIO_ARGS <<< "${scenario_line}"
FRAMES="${FRAMES:-0}"
SCENARIO_ARGS="${SCENARIO_ARGS:-}"

if [[ -z "${ROM_PATH}" || ! -f "${ROM_PATH}" ]]; then
  echo "ERROR: ROM path for scenario '${SCENARIO_ID}' is missing or does not exist: ${ROM_PATH}" >&2
  exit 2
fi

if ! [[ "${FRAMES}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: frames value for scenario '${SCENARIO_ID}' is invalid: ${FRAMES}" >&2
  exit 2
fi

mkdir -p "${CACHE_ROOT}" "${RUN_ROOT}"

REFERENCE_CAPTURE_METHOD_TAG="dumpfb_flip${DUMPFB_FLIP_Y}"
REFERENCE_CAPTURE="${CACHE_ROOT}/${SCENARIO_ID}.${REFERENCE_CAPTURE_METHOD_TAG}.reference.ppm"
CANDIDATE_CAPTURE="${RUN_ROOT}/${SCENARIO_ID}.candidate.ppm"
DIFF_OUT="${RUN_ROOT}/${SCENARIO_ID}.diff.png"
METRICS_OUT="${RUN_ROOT}/${SCENARIO_ID}.metrics.json"
CAPTURE_CONTEXT_OUT="${RUN_ROOT}/${SCENARIO_ID}.capture-context.json"
REFERENCE_PNG="${RUN_ROOT}/${SCENARIO_ID}.reference.png"
CANDIDATE_PNG="${RUN_ROOT}/${SCENARIO_ID}.candidate.png"

CANDIDATE_CAPTURE_METHOD_TAG="dumpfb_flip${DUMPFB_FLIP_Y}"

SCENARIO_ARGS_ARRAY=()
if [[ -n "${SCENARIO_ARGS// }" ]]; then
  # shellcheck disable=SC2206
  SCENARIO_ARGS_ARRAY=(${SCENARIO_ARGS})
fi

capture_plugin() {
  local label="$1"
  local plugin_path="$2"
  local out_path="$3"
  local corelib_path="${CANDIDATE_CORELIB}"
  local require_no_depth_fail="0"
  local require_depth_stats="0"
  local depth_summary_out=""
  if [[ "${label}" == "reference" ]]; then
    corelib_path="${REFERENCE_CORELIB}"
  fi
  if [[ "${label}" == "candidate" ]]; then
    require_no_depth_fail="${REQUIRE_NO_DEPTH_BLIT_FAIL}"
    require_depth_stats="${REQUIRE_DEPTH_BLIT_STATS}"
  fi
  if [[ "${CAPTURE_DEPTH_SUMMARY}" == "1" ]]; then
    depth_summary_out="${RUN_ROOT}/${SCENARIO_ID}.${label}.depth-blit-summary.json"
  fi

  local -a cmd=(
    "${ROOT_DIR}/scripts/paper_mario_smoke_runner.sh"
    --backend "Vulkan"
    --rom "${ROM_PATH}"
    --frames "${FRAMES}"
    --out "${out_path}"
  )
  if ((${#SCENARIO_ARGS_ARRAY[@]})); then
    cmd+=("${SCENARIO_ARGS_ARRAY[@]}")
  fi

  M64_CORELIB="${corelib_path}" \
  REALITYVK_SMOKE_PLUGIN_VULKAN="${plugin_path}" \
  REALITYVK_SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL="${require_no_depth_fail}" \
  REALITYVK_SMOKE_REQUIRE_DEPTH_BLIT_STATS="${require_depth_stats}" \
  REALITYVK_SMOKE_DEPTH_BLIT_SUMMARY_OUT="${depth_summary_out}" \
  REALITYVK_SMOKE_REQUIRE_NON_BLACK_CAPTURE="${REQUIRE_NON_BLACK_CAPTURE}" \
  REALITYVK_SMOKE_CAPTURE_RETRY_COUNT="${CAPTURE_RETRY_COUNT}" \
  REALITYVK_SMOKE_CAPTURE_RETRY_STEP_FRAMES="${CAPTURE_RETRY_STEP_FRAMES}" \
  REALITYVK_SMOKE_CAPTURE_RETRY_RESUME_MS="${CAPTURE_RETRY_RESUME_MS}" \
  REALITYVK_SMOKE_CAPTURE_MIN_NONBLACK_RATIO="${CAPTURE_MIN_NONBLACK_RATIO}" \
  REALITYVK_SMOKE_CAPTURE_MIN_MEAN_LUMA="${CAPTURE_MIN_MEAN_LUMA}" \
  REALITYVK_SMOKE_DUMPFB_FLIP_Y="${DUMPFB_FLIP_Y}" \
  REALITYVK_SMOKE_LAUNCH_WITH_PTY="${LAUNCH_WITH_PTY}" \
  "${cmd[@]}"
}

if [[ "${REFRESH_REFERENCE}" == "1" || ! -s "${REFERENCE_CAPTURE}" ]]; then
  if [[ ! -f "${REFERENCE_PLUGIN}" ]]; then
    echo "ERROR: reference plugin not found: ${REFERENCE_PLUGIN}" >&2
    exit 2
  fi
  echo "==> [compare] capturing reference (${SCENARIO_ID})"
  echo "    plugin: ${REFERENCE_PLUGIN}"
  echo "    core:   ${REFERENCE_CORELIB}"
  echo "    capture:${REFERENCE_CAPTURE_METHOD_TAG}"
  capture_plugin "reference" "${REFERENCE_PLUGIN}" "${REFERENCE_CAPTURE}"
else
  echo "==> [compare] using cached reference capture: ${REFERENCE_CAPTURE}"
fi

echo "==> [compare] capturing candidate (${SCENARIO_ID})"
echo "    plugin: ${CANDIDATE_PLUGIN}"
echo "    core:   ${CANDIDATE_CORELIB}"
echo "    capture:${CANDIDATE_CAPTURE_METHOD_TAG}"
capture_plugin "candidate" "${CANDIDATE_PLUGIN}" "${CANDIDATE_CAPTURE}"

python3 - "${CAPTURE_CONTEXT_OUT}" \
  "${SCENARIO_ID}" \
  "${REFERENCE_PLUGIN}" \
  "${REFERENCE_CORELIB}" \
  "${REFERENCE_CAPTURE_METHOD_TAG}" \
  "${REFERENCE_CAPTURE}" \
  "${CANDIDATE_PLUGIN}" \
  "${CANDIDATE_CORELIB}" \
  "${CANDIDATE_CAPTURE_METHOD_TAG}" \
  "${CANDIDATE_CAPTURE}" <<'PY'
import json
import sys
from pathlib import Path

out_path = Path(sys.argv[1])
payload = {
    "scenario_id": sys.argv[2],
    "reference": {
        "plugin": sys.argv[3],
        "corelib": sys.argv[4],
        "capture_method": sys.argv[5],
        "capture_path": sys.argv[6],
    },
    "candidate": {
        "plugin": sys.argv[7],
        "corelib": sys.argv[8],
        "capture_method": sys.argv[9],
        "capture_path": sys.argv[10],
    },
}
out_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
print(f"capture context: {out_path}")
PY

python3 - "${REFERENCE_CAPTURE}" "${CANDIDATE_CAPTURE}" "${DIFF_OUT}" "${METRICS_OUT}" "${RMSE_MAX}" "${MAE_MAX}" "${VISUAL_GATE}" <<'PY'
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ref_path = Path(sys.argv[1])
test_path = Path(sys.argv[2])
diff_path = Path(sys.argv[3])
metrics_path = Path(sys.argv[4])
rmse_limit = float(sys.argv[5]) if sys.argv[5] else None
mae_limit = float(sys.argv[6]) if sys.argv[6] else None
gate_enabled = sys.argv[7] == "1"

ref = np.asarray(Image.open(ref_path).convert("RGB"), dtype=np.float32) / 255.0
test = np.asarray(Image.open(test_path).convert("RGB"), dtype=np.float32) / 255.0

if ref.shape != test.shape:
    raise SystemExit(
        f"ERROR: capture shape mismatch: reference={ref.shape}, candidate={test.shape}"
    )

abs_diff = np.abs(ref - test)
rmse = float(np.sqrt(np.mean((ref - test) ** 2)))
mae = float(np.mean(abs_diff))
max_diff = float(np.max(abs_diff))

diff_u8 = np.clip(abs_diff * 255.0, 0.0, 255.0).astype(np.uint8)
Image.fromarray(diff_u8, mode="RGB").save(diff_path)

metrics = {
    "reference_capture": str(ref_path),
    "candidate_capture": str(test_path),
    "diff_image": str(diff_path),
    "rmse": rmse,
    "mae": mae,
    "max_abs_diff": max_diff,
    "rmse_limit": rmse_limit,
    "mae_limit": mae_limit,
    "gate_enabled": gate_enabled,
}

violations = []
if gate_enabled:
    if rmse_limit is not None and rmse > rmse_limit:
        violations.append(f"rmse {rmse:.6f} > limit {rmse_limit:.6f}")
    if mae_limit is not None and mae > mae_limit:
        violations.append(f"mae {mae:.6f} > limit {mae_limit:.6f}")

metrics["pass"] = len(violations) == 0
metrics["violations"] = violations
metrics_path.write_text(json.dumps(metrics, indent=2), encoding="utf-8")

print(f"visual compare metrics: rmse={rmse:.6f} mae={mae:.6f} max_abs_diff={max_diff:.6f}")
print(f"metrics json: {metrics_path}")
print(f"diff image: {diff_path}")

if violations:
    for item in violations:
        print(f"ERROR: {item}", file=sys.stderr)
    raise SystemExit(1)
PY

python3 - "${REFERENCE_CAPTURE}" "${CANDIDATE_CAPTURE}" "${REFERENCE_PNG}" "${CANDIDATE_PNG}" <<'PY'
from pathlib import Path
from PIL import Image
import sys

ref_ppm = Path(sys.argv[1])
cand_ppm = Path(sys.argv[2])
ref_png = Path(sys.argv[3])
cand_png = Path(sys.argv[4])

ref_png.parent.mkdir(parents=True, exist_ok=True)
Image.open(ref_ppm).convert("RGB").save(ref_png)
Image.open(cand_ppm).convert("RGB").save(cand_png)
print(f"reference png: {ref_png}")
print(f"candidate png: {cand_png}")
PY

if [[ "${CAPTURE_DEPTH_SUMMARY}" == "1" ]]; then
  ref_depth_summary="${RUN_ROOT}/${SCENARIO_ID}.reference.depth-blit-summary.json"
  cand_depth_summary="${RUN_ROOT}/${SCENARIO_ID}.candidate.depth-blit-summary.json"
  if [[ -f "${ref_depth_summary}" ]]; then
    echo "depth blit summary (reference): ${ref_depth_summary}"
  fi
  if [[ -f "${cand_depth_summary}" ]]; then
    echo "depth blit summary (candidate): ${cand_depth_summary}"
  fi
fi
