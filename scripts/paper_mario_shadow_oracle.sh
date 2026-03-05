#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT_DIR}/scripts/lib/validation.sh"
source "${ROOT_DIR}/scripts/lib/compare_view.sh"

SCENARIO_ID="${REALITYVK_PM_SCENARIO_ID:-paper_mario_intro}"
FRAMES_OVERRIDE="${REALITYVK_PM_FRAMES_OVERRIDE:-20}"
PROFILE="${REALITYVK_PM_PROFILE:-deep}"
RUN_ROOT_BASE="${REALITYVK_PM_SHADOW_ORACLE_RUN_ROOT:-${ROOT_DIR}/build/parity-runs/paper-mario/shadow-oracle}"
RETRY_COUNT="${REALITYVK_PM_SHADOW_ORACLE_RETRY_COUNT:-0}"
DIFF_MODE="${REALITYVK_PM_SHADOW_ORACLE_DIFF_MODE:-missing_non_black}"
THRESHOLD="${REALITYVK_PM_SHADOW_ORACLE_THRESHOLD:-20}"
MIN_AREA="${REALITYVK_PM_SHADOW_ORACLE_MIN_AREA:-128}"
HISTORY_FRAME_WINDOW="${REALITYVK_PM_SHADOW_ORACLE_HISTORY_FRAME_WINDOW:-6}"
CLOSE_ALL_EOG="${REALITYVK_PM_AUTO_COMPARE_CLOSE_ALL_EOG:-1}"
AUTO_OPEN="${REALITYVK_PM_SHADOW_ORACLE_AUTO_OPEN:-1}"
IGNORE_BOXES=("${REALITYVK_PM_SHADOW_ORACLE_IGNORE_BOX:-238,245,482,380}")
DRY_RUN="${REALITYVK_PM_DRY_RUN:-0}"
DRY_RUN_OUT="${REALITYVK_PM_DRY_RUN_OUT:-}"

usage() {
  cat <<'EOF_USAGE'
Usage:
  paper_mario_shadow_oracle.sh [options]

Options:
  --scenario-id <id>              Scenario id (default: paper_mario_intro)
  --frames <n>                    Frame override for both runs (default: 20)
  --profile <basic|deep>          Parity profile (default: deep)
  --run-root <dir>                Run root (default: build/parity-runs/paper-mario/shadow-oracle)
  --retry-count <n>               Capture retry count for deterministic pairing (default: 0)
  --mode <missing_non_black|absdiff|extra_non_black>
                                  Diff mode passed to rvk2_image_diff_playbook.py (default: missing_non_black)
  --threshold <0..255>            Diff threshold (default: 20)
  --min-area <n>                  Minimum diff component area (default: 128)
  --history-frame-window <n>      Missing-region focus history window (default: 6)
  --ignore-box <x0,y0,x1,y1>      Ignore box for diff (repeatable)
  --auto-open <0|1>               Open side-by-side oracle image in eog (default: 1)
  --close-all-eog <0|1>           Close all eog windows before open (default: 1)
  --dry-run                       Emit planned commands and exit
  --dry-run-out <path>            Optional dry-run JSON output path
  -h, --help                      Show this help
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
    --retry-count)
      RETRY_COUNT="$2"
      shift 2
      ;;
    --mode)
      DIFF_MODE="$2"
      shift 2
      ;;
    --threshold)
      THRESHOLD="$2"
      shift 2
      ;;
    --min-area)
      MIN_AREA="$2"
      shift 2
      ;;
    --history-frame-window)
      HISTORY_FRAME_WINDOW="$2"
      shift 2
      ;;
    --ignore-box)
      IGNORE_BOXES+=("$2")
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
    --dry-run)
      DRY_RUN="1"
      shift
      ;;
    --dry-run-out)
      DRY_RUN_OUT="$2"
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
rvk2_require_enum "--mode" "${DIFF_MODE}" "missing_non_black" "absdiff" "extra_non_black"
rvk2_require_uint_ge "--frames" "${FRAMES_OVERRIDE}" 0
rvk2_require_uint_ge "--retry-count" "${RETRY_COUNT}" 0
rvk2_require_uint_ge "--threshold" "${THRESHOLD}" 0
rvk2_require_uint_ge "--min-area" "${MIN_AREA}" 0
rvk2_require_uint_ge "--history-frame-window" "${HISTORY_FRAME_WINDOW}" 0
rvk2_require_bool "--auto-open" "${AUTO_OPEN}"
rvk2_require_bool "--close-all-eog" "${CLOSE_ALL_EOG}"
rvk2_require_bool "--dry-run" "${DRY_RUN}"

stamp="$(date -u +%Y%m%d-%H%M%SZ)"
run_root="${RUN_ROOT_BASE}/${stamp}"
off_root="${run_root}/off"
on_root="${run_root}/on"
oracle_root="${run_root}/oracle-compare"
mkdir -p "${off_root}" "${on_root}" "${oracle_root}"

build_diff_args() {
  local -n _out="$1"
  _out=(--mode "${DIFF_MODE}" --threshold "${THRESHOLD}" --min-area "${MIN_AREA}")
  for box in "${IGNORE_BOXES[@]}"; do
    if [[ -n "${box}" ]]; then
      _out+=(--ignore-box "${box}")
    fi
  done
}

run_shadow_case() {
  local shadow_draw="$1"
  local shadow_present="$2"
  local run_root_case="$3"

  env \
    REALITYVK_PM_SCENARIO_ID="${SCENARIO_ID}" \
    REALITYVK_PM_PROFILE="${PROFILE}" \
    REALITYVK_PM_VISUAL_GATE=0 \
    REALITYVK_PM_AUTO_COMPARE_VIEW=0 \
    REALITYVK_PM_KNOB_TRACK_ENABLE=0 \
    REALITYVK_PM_DEEP_TELEMETRY=1 \
    REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE=1 \
    REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG=1 \
    REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_LIMIT=3000000 \
    REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_ALL_WRITES=1 \
    REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_TEXEL_DETAIL=1 \
    REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_FROM_LAST_FOCUS=0 \
    REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_IDS= \
    REALITYVK_PM_DEEP_TELEMETRY_TRIANGLE_PACKET_LOG=1 \
    REALITYVK_PM_DEEP_TELEMETRY_COMMAND_CENSUS=1 \
    REALITYVK_PM_FRAMES_OVERRIDE="${FRAMES_OVERRIDE}" \
    REALITYVK_PM_RUN_ROOT="${run_root_case}" \
    REALITYVK_SMOKE_CAPTURE_RETRY_COUNT="${RETRY_COUNT}" \
    REALITYVK_RVK2_SHADOW_DRAW="${shadow_draw}" \
    REALITYVK_RVK2_SHADOW_PRESENT="${shadow_present}" \
    "${ROOT_DIR}/scripts/paper_mario_parity.sh"
}

resolve_shadow_run_telemetry_artifact() {
  local run_root_case="$1"
  local suffix="$2"
  local local_path="${run_root_case}/telemetry/${SCENARIO_ID}.candidate.${suffix}"
  if [[ -f "${local_path}" ]]; then
    printf '%s\n' "${local_path}"
    return 0
  fi

  local latest_archive="${run_root_case}/archive/${SCENARIO_ID}.latest"
  local archive_path="${latest_archive}/telemetry/${SCENARIO_ID}.candidate.${suffix}"
  if [[ -f "${archive_path}" ]]; then
    printf '%s\n' "${archive_path}"
    return 0
  fi

  return 1
}

if [[ "${DRY_RUN}" == "1" ]]; then
  python3 - \
    "${DRY_RUN_OUT}" \
    "${SCENARIO_ID}" \
    "${PROFILE}" \
    "${FRAMES_OVERRIDE}" \
    "${RETRY_COUNT}" \
    "${DIFF_MODE}" \
    "${THRESHOLD}" \
    "${MIN_AREA}" \
    "${HISTORY_FRAME_WINDOW}" \
    "${run_root}" \
    "${off_root}" \
    "${on_root}" \
    "${oracle_root}" \
    "$(IFS=';'; echo "${IGNORE_BOXES[*]}")" <<'PY'
import json
import sys
from pathlib import Path

out = sys.argv[1]
payload = {
    "schema": "paper_mario_shadow_oracle_dry_run_v1",
    "scenario_id": sys.argv[2],
    "profile": sys.argv[3],
    "frames": int(sys.argv[4]),
    "retry_count": int(sys.argv[5]),
    "diff_mode": sys.argv[6],
    "threshold": int(sys.argv[7]),
    "min_area": int(sys.argv[8]),
    "history_frame_window": int(sys.argv[9]),
    "run_root": sys.argv[10],
    "off_root": sys.argv[11],
    "on_root": sys.argv[12],
    "oracle_root": sys.argv[13],
    "ignore_boxes": [box for box in sys.argv[14].split(";") if box],
}
if out:
    out_path = Path(out)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(out_path)
else:
    print(json.dumps(payload, indent=2))
PY
  exit 0
fi

echo "==> [shadow-oracle] off: shadow_draw=0 shadow_present=0 profile=${PROFILE} frames=${FRAMES_OVERRIDE} retry=${RETRY_COUNT}"
run_shadow_case "0" "0" "${off_root}"

echo "==> [shadow-oracle] on: shadow_draw=1 shadow_present=1 profile=${PROFILE} frames=${FRAMES_OVERRIDE} retry=${RETRY_COUNT}"
run_shadow_case "1" "1" "${on_root}"

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
rvk2_require_file "shadow-off candidate artifact" "${off_png}"
rvk2_require_file "shadow-on candidate artifact" "${on_png}"

side_by_side_out="${oracle_root}/${SCENARIO_ID}.shadow_off_vs_on.latest.png"
viewer_pid_file="${RUN_ROOT_BASE}/.shadow_oracle_compare_view.pid"
rvk2_build_side_by_side_image "${off_png}" "${on_png}" "${side_by_side_out}"

if [[ "${AUTO_OPEN}" == "1" ]]; then
  rvk2_close_eog_view "${viewer_pid_file}" "${CLOSE_ALL_EOG}"
  rvk2_open_eog_view "${side_by_side_out}" "${viewer_pid_file}"
fi

diff_args=()
build_diff_args diff_args
python3 "${ROOT_DIR}/scripts/rvk2_image_diff_playbook.py" \
  --ref "${on_png}" \
  --test "${off_png}" \
  --outdir "${oracle_root}" \
  "${diff_args[@]}"

off_packet_trace=""
off_forensics=""
off_triangle_log=""
if off_packet_trace="$(resolve_shadow_run_telemetry_artifact "${off_root}" "packet.tsv")"; then
  :
else
  off_packet_trace=""
fi
if off_forensics="$(resolve_shadow_run_telemetry_artifact "${off_root}" "frame-forensics.tsv")"; then
  :
else
  off_forensics=""
fi
if off_triangle_log="$(resolve_shadow_run_telemetry_artifact "${off_root}" "triangle-packet.tsv")"; then
  :
else
  off_triangle_log=""
fi
oracle_diff_summary="${oracle_root}/summary.json"
oracle_missing_focus="${oracle_root}/missing-region-focus.off-vs-shadow.json"

if [[ -n "${off_packet_trace}" && -f "${off_packet_trace}" && -f "${oracle_diff_summary}" ]]; then
  focus_cmd=(
    python3 "${ROOT_DIR}/scripts/rvk2_missing_region_focus.py"
    --packet-trace "${off_packet_trace}"
    --diff-summary "${oracle_diff_summary}"
    --output "${oracle_missing_focus}"
    --history-frame-window "${HISTORY_FRAME_WINDOW}"
  )
  if [[ -n "${off_forensics}" && -f "${off_forensics}" ]]; then
    focus_cmd+=(--forensics "${off_forensics}")
  fi
  if [[ -n "${off_triangle_log}" && -f "${off_triangle_log}" ]]; then
    focus_cmd+=(--triangle-packet-log "${off_triangle_log}")
  fi
  "${focus_cmd[@]}"
else
  echo "WARN: skipping missing-region focus (off packet trace or oracle diff summary not found)." >&2
fi

python3 - \
  "${off_root}/${SCENARIO_ID}.metrics.json" \
  "${on_root}/${SCENARIO_ID}.metrics.json" \
  "${oracle_diff_summary}" \
  "${oracle_missing_focus}" <<'PY'
import json
import sys
from pathlib import Path

off_metrics_path = Path(sys.argv[1])
on_metrics_path = Path(sys.argv[2])
summary_path = Path(sys.argv[3])
focus_path = Path(sys.argv[4])

def read_json(path: Path):
    if not path.is_file():
        return {}
    return json.loads(path.read_text(encoding="utf-8"))

off = read_json(off_metrics_path)
on = read_json(on_metrics_path)
summary = read_json(summary_path)
focus = read_json(focus_path)
missing = focus.get("missing_write_attribution", {})

print("==> [shadow-oracle] summary")
print(f"off candidate_non_black_ratio={off.get('candidate_non_black_ratio')}")
print(f"on  candidate_non_black_ratio={on.get('candidate_non_black_ratio')}")
print(f"off rmse={off.get('rmse')} mae={off.get('mae')}")
print(f"on  rmse={on.get('rmse')} mae={on.get('mae')}")
if summary:
    print(f"diff bbox_count={summary.get('bbox_count')} first_mismatch={summary.get('first_mismatch')}")
if missing:
    print(f"missing_with_write_ratio={missing.get('missing_with_write_ratio')}")
    print(f"missing_without_write_ratio={missing.get('missing_without_write_ratio')}")
    print(
        "missing_without_write_with_prior_write_ratio="
        f"{missing.get('missing_without_write_with_prior_write_ratio')}"
    )
    top_hits = missing.get("missing_with_write_packet_hits", [])
    print("top_missing_with_write_packets=")
    for row in top_hits[:8]:
        print(
            f"  packet={row.get('source_packet_id')} frame={row.get('frame_id')} "
            f"op={row.get('op_kind')} pixels={row.get('pixel_hits')} "
            f"combine={row.get('combine_mux')} other={row.get('other_modes')}"
        )

PY

echo "shadow-oracle run root: ${run_root}"
echo "shadow off candidate: ${off_png}"
echo "shadow on candidate: ${on_png}"
echo "shadow off vs on image: ${side_by_side_out}"
echo "oracle diff summary: ${oracle_diff_summary}"
if [[ -f "${oracle_missing_focus}" ]]; then
  echo "oracle missing-region focus: ${oracle_missing_focus}"
fi
