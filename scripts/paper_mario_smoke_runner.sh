#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

BACKEND="Vulkan"
ROM_PATH=""
OUT_PATH=""
FRAMES="0"
STATE_PATH=""
PRESET="full"
SCALE_DIV="1"
SOCKET_PATH="${M64_AGENT_SOCKET:-/tmp/m64-agent-smoke.sock}"
AGENT_PROFILE="${M64_AGENT_PROFILE:-watch}"
RUNTIME_ROOT="${REALITYVK_SMOKE_M64_ROOT:-/home/auro/code/mupen}"
RUNTIME_DIR="${RUNTIME_ROOT}/mupen64plus-runtime"
AGENTCTL="${RUNTIME_DIR}/agentctl.py"
LAUNCH_SCRIPT="${RUNTIME_DIR}/launch.sh"
PLUGIN_DIR="${RUNTIME_DIR}/plugins"
DEFAULT_PLUGIN="${ROOT_DIR}/build/local-gate/linux-release-cli/plugin/Release/mupen64plus-video-RealityVK.so"
AGENTCTL_TIMEOUT_SEC="${REALITYVK_SMOKE_AGENTCTL_TIMEOUT_SEC:-30}"
STEP_CHUNK="${REALITYVK_SMOKE_STEP_CHUNK:-1}"
SETTLE_FRAMES_AFTER_LOAD="${REALITYVK_SMOKE_SETTLE_FRAMES_AFTER_LOAD:-1}"
LAUNCH_QUIET="${REALITYVK_SMOKE_LAUNCH_QUIET:-1}"
LAUNCH_WITH_PTY="${REALITYVK_SMOKE_LAUNCH_WITH_PTY:-0}"
REQUIRE_READBACK_MARKER="${REALITYVK_SMOKE_REQUIRE_READBACK_MARKER:-0}"
READBACK_MARKER_REGEX="${REALITYVK_SMOKE_READBACK_MARKER_REGEX:-VK readback debug:}"
REQUIRE_NO_DEPTH_BLIT_FAIL="${REALITYVK_SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL:-0}"
REQUIRE_DEPTH_BLIT_STATS="${REALITYVK_SMOKE_REQUIRE_DEPTH_BLIT_STATS:-0}"
DEPTH_BLIT_FAIL_REGEX="${REALITYVK_SMOKE_DEPTH_BLIT_FAIL_REGEX:-op=blit_depth_fail}"
DEPTH_BLIT_SUMMARY_OUT="${REALITYVK_SMOKE_DEPTH_BLIT_SUMMARY_OUT:-}"
REQUIRE_NON_BLACK_CAPTURE="${REALITYVK_SMOKE_REQUIRE_NON_BLACK_CAPTURE:-0}"
CAPTURE_RETRY_COUNT="${REALITYVK_SMOKE_CAPTURE_RETRY_COUNT:-6}"
CAPTURE_RETRY_STEP_FRAMES="${REALITYVK_SMOKE_CAPTURE_RETRY_STEP_FRAMES:-20}"
CAPTURE_RETRY_RESUME_MS="${REALITYVK_SMOKE_CAPTURE_RETRY_RESUME_MS:-250}"
CAPTURE_MIN_NONBLACK_RATIO="${REALITYVK_SMOKE_CAPTURE_MIN_NONBLACK_RATIO:-0.001}"
CAPTURE_MIN_MEAN_LUMA="${REALITYVK_SMOKE_CAPTURE_MIN_MEAN_LUMA:-0.002}"
CAPTURE_DEBUG="${REALITYVK_SMOKE_CAPTURE_DEBUG:-0}"
DUMPFB_FLIP_Y="${REALITYVK_SMOKE_DUMPFB_FLIP_Y:-0}"
CAPTURE_METHOD="${REALITYVK_SMOKE_CAPTURE_METHOD:-dumpfb-preset}"
SCREENSHOT_DIR="${REALITYVK_SMOKE_SCREENSHOT_DIR:-${HOME}/.local/share/mupen64plus/screenshot}"
SCREENSHOT_FLIP_Y="${REALITYVK_SMOKE_SCREENSHOT_FLIP_Y:-auto}"
SCREENSHOT_FLIP_Y_EFFECTIVE=""

usage() {
  cat <<EOF_USAGE
Usage:
  $0 --backend <name> --rom <path> --frames <count> --out <path> [options]

Options:
  --state <path>       Optional savestate to load before stepping/capture.
  --preset <name>      Framebuffer preset for agent dump (default: full).
  --scale-div <n>      Downscale factor for capture (default: 1).
  --socket <path>      Agent socket path (default: ${SOCKET_PATH}).
EOF_USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --backend)
      BACKEND="$2"
      shift 2
      ;;
    --rom)
      ROM_PATH="$2"
      shift 2
      ;;
    --frames)
      FRAMES="$2"
      shift 2
      ;;
    --out)
      OUT_PATH="$2"
      shift 2
      ;;
    --state)
      STATE_PATH="$2"
      shift 2
      ;;
    --preset)
      PRESET="$2"
      shift 2
      ;;
    --scale-div)
      SCALE_DIV="$2"
      shift 2
      ;;
    --socket)
      SOCKET_PATH="$2"
      shift 2
      ;;
    --help|-h)
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

if [[ "${BACKEND}" != "Vulkan" ]]; then
  echo "ERROR: paper_mario_smoke_runner.sh supports only --backend Vulkan." >&2
  exit 2
fi

if [[ -z "${ROM_PATH}" ]]; then
  echo "ERROR: --rom is required." >&2
  exit 2
fi

if [[ -z "${OUT_PATH}" ]]; then
  echo "ERROR: --out is required." >&2
  exit 2
fi

if ! [[ "${FRAMES}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: --frames must be an integer >= 0." >&2
  exit 2
fi

if ! [[ "${STEP_CHUNK}" =~ ^[0-9]+$ ]] || [[ "${STEP_CHUNK}" == "0" ]]; then
  echo "ERROR: REALITYVK_SMOKE_STEP_CHUNK must be an integer >= 1." >&2
  exit 2
fi

if ! [[ "${SETTLE_FRAMES_AFTER_LOAD}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_SMOKE_SETTLE_FRAMES_AFTER_LOAD must be an integer >= 0." >&2
  exit 2
fi

if ! [[ "${SCALE_DIV}" =~ ^[0-9]+$ ]] || [[ "${SCALE_DIV}" == "0" ]]; then
  echo "ERROR: --scale-div must be an integer >= 1." >&2
  exit 2
fi

if [[ ! -f "${ROM_PATH}" ]]; then
  echo "ERROR: ROM not found: ${ROM_PATH}" >&2
  exit 2
fi

if [[ -n "${STATE_PATH}" && ! -f "${STATE_PATH}" ]]; then
  echo "ERROR: state file not found: ${STATE_PATH}" >&2
  exit 2
fi

if [[ ! -x "${LAUNCH_SCRIPT}" ]]; then
  echo "ERROR: launch script not executable: ${LAUNCH_SCRIPT}" >&2
  exit 2
fi

if [[ ! -f "${AGENTCTL}" ]]; then
  echo "ERROR: agentctl not found: ${AGENTCTL}" >&2
  exit 2
fi

backend_plugin="${REALITYVK_SMOKE_PLUGIN_VULKAN:-${REALITYVK_SMOKE_PLUGIN:-${DEFAULT_PLUGIN}}}"
if [[ ! -f "${backend_plugin}" ]]; then
  echo "ERROR: Vulkan plugin binary not found: ${backend_plugin}" >&2
  exit 2
fi
backend_plugin_name="$(basename "${backend_plugin}")"
PLUGIN_DEST="${PLUGIN_DIR}/${backend_plugin_name}"

if [[ "${REQUIRE_READBACK_MARKER}" != "0" && "${REQUIRE_READBACK_MARKER}" != "1" ]]; then
  echo "ERROR: REALITYVK_SMOKE_REQUIRE_READBACK_MARKER must be 0 or 1." >&2
  exit 2
fi

if [[ "${REQUIRE_NO_DEPTH_BLIT_FAIL}" != "0" && "${REQUIRE_NO_DEPTH_BLIT_FAIL}" != "1" ]]; then
  echo "ERROR: REALITYVK_SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL must be 0 or 1." >&2
  exit 2
fi

if [[ "${REQUIRE_DEPTH_BLIT_STATS}" != "0" && "${REQUIRE_DEPTH_BLIT_STATS}" != "1" ]]; then
  echo "ERROR: REALITYVK_SMOKE_REQUIRE_DEPTH_BLIT_STATS must be 0 or 1." >&2
  exit 2
fi

if [[ "${REQUIRE_NON_BLACK_CAPTURE}" != "0" && "${REQUIRE_NON_BLACK_CAPTURE}" != "1" ]]; then
  echo "ERROR: REALITYVK_SMOKE_REQUIRE_NON_BLACK_CAPTURE must be 0 or 1." >&2
  exit 2
fi

if [[ "${CAPTURE_DEBUG}" != "0" && "${CAPTURE_DEBUG}" != "1" ]]; then
  echo "ERROR: REALITYVK_SMOKE_CAPTURE_DEBUG must be 0 or 1." >&2
  exit 2
fi

if [[ "${DUMPFB_FLIP_Y}" != "0" && "${DUMPFB_FLIP_Y}" != "1" ]]; then
  echo "ERROR: REALITYVK_SMOKE_DUMPFB_FLIP_Y must be 0 or 1." >&2
  exit 2
fi

if [[ "${SCREENSHOT_FLIP_Y}" != "0" && "${SCREENSHOT_FLIP_Y}" != "1" && "${SCREENSHOT_FLIP_Y}" != "auto" ]]; then
  echo "ERROR: REALITYVK_SMOKE_SCREENSHOT_FLIP_Y must be 0, 1, or auto." >&2
  exit 2
fi

if [[ "${SCREENSHOT_FLIP_Y}" == "auto" ]]; then
  # Align screenshot orientation with dumpfb output by default.
  if [[ "${DUMPFB_FLIP_Y}" == "1" ]]; then
    SCREENSHOT_FLIP_Y_EFFECTIVE="0"
  else
    SCREENSHOT_FLIP_Y_EFFECTIVE="1"
  fi
else
  SCREENSHOT_FLIP_Y_EFFECTIVE="${SCREENSHOT_FLIP_Y}"
fi

if [[ "${CAPTURE_METHOD}" != "dumpfb-preset" && "${CAPTURE_METHOD}" != "screenshot" ]]; then
  echo "ERROR: REALITYVK_SMOKE_CAPTURE_METHOD must be 'dumpfb-preset' or 'screenshot'." >&2
  exit 2
fi

if [[ "${CAPTURE_METHOD}" == "screenshot" && "${PRESET}" != "full" ]]; then
  echo "ERROR: screenshot capture supports only --preset full." >&2
  exit 2
fi

if [[ "${LAUNCH_WITH_PTY}" != "0" && "${LAUNCH_WITH_PTY}" != "1" ]]; then
  echo "ERROR: REALITYVK_SMOKE_LAUNCH_WITH_PTY must be 0 or 1." >&2
  exit 2
fi

if ! [[ "${CAPTURE_RETRY_COUNT}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_SMOKE_CAPTURE_RETRY_COUNT must be an integer >= 0." >&2
  exit 2
fi

if ! [[ "${CAPTURE_RETRY_STEP_FRAMES}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_SMOKE_CAPTURE_RETRY_STEP_FRAMES must be an integer >= 0." >&2
  exit 2
fi

if ! [[ "${CAPTURE_RETRY_RESUME_MS}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_SMOKE_CAPTURE_RETRY_RESUME_MS must be an integer >= 0." >&2
  exit 2
fi

if ! [[ "${CAPTURE_MIN_NONBLACK_RATIO}" =~ ^[0-9]*\.?[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_SMOKE_CAPTURE_MIN_NONBLACK_RATIO must be numeric." >&2
  exit 2
fi

if ! [[ "${CAPTURE_MIN_MEAN_LUMA}" =~ ^[0-9]*\.?[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_SMOKE_CAPTURE_MIN_MEAN_LUMA must be numeric." >&2
  exit 2
fi

mkdir -p "${PLUGIN_DIR}"
cp "${backend_plugin}" "${PLUGIN_DEST}"
chmod 755 "${PLUGIN_DEST}"
mkdir -p "$(dirname "${OUT_PATH}")"

run_agentctl() {
  local -a cmd=(python3 "${AGENTCTL}" --socket "${SOCKET_PATH}" "$@")
  if command -v timeout >/dev/null 2>&1; then
    timeout "${AGENTCTL_TIMEOUT_SEC}" "${cmd[@]}"
  else
    "${cmd[@]}"
  fi
}

step_frames() {
  local frames="$1"
  if [[ "${frames}" == "0" ]]; then
    return
  fi
  local remaining="${frames}"
  while (( remaining > 0 )); do
    local step_now="${STEP_CHUNK}"
    if (( remaining < step_now )); then
      step_now="${remaining}"
    fi
    if run_agentctl step "${step_now}" >/dev/null; then
      remaining="$(( remaining - step_now ))"
      continue
    fi

    if (( step_now > 1 )); then
      echo "WARN: batched step ${step_now} timed out; switching to single-step recovery." >&2
      local idx=0
      while (( idx < step_now )); do
        run_agentctl step 1 >/dev/null
        idx="$(( idx + 1 ))"
      done
      remaining="$(( remaining - step_now ))"
      continue
    fi

    return 1
  done
}

sleep_ms() {
  local millis="$1"
  if [[ "${millis}" == "0" ]]; then
    return
  fi
  python3 - "${millis}" <<'PY'
import sys
import time

millis = int(sys.argv[1])
if millis > 0:
    time.sleep(float(millis) / 1000.0)
PY
}

capture_has_content() {
  local capture_file="$1"
  python3 - "${capture_file}" "${CAPTURE_MIN_NONBLACK_RATIO}" "${CAPTURE_MIN_MEAN_LUMA}" "${CAPTURE_DEBUG}" <<'PY'
import re
import sys
from pathlib import Path

capture_path = Path(sys.argv[1])
min_nonblack_ratio = float(sys.argv[2])
min_mean_luma = float(sys.argv[3])
debug = sys.argv[4] == "1"

if not capture_path.exists() or capture_path.stat().st_size == 0:
    raise SystemExit(2)

data = capture_path.read_bytes()
index = 0
data_len = len(data)

def next_token():
    global index
    while index < data_len:
        ch = data[index]
        if ch in b" \t\r\n":
            index += 1
            continue
        if ch == ord("#"):
            while index < data_len and data[index] not in b"\r\n":
                index += 1
            continue
        break
    if index >= data_len:
        return None
    start = index
    while index < data_len and data[index] not in b" \t\r\n":
        index += 1
    return data[start:index]

magic = next_token()
if magic not in (b"P6", b"P3"):
    raise SystemExit(2)

width_token = next_token()
height_token = next_token()
maxval_token = next_token()
if width_token is None or height_token is None or maxval_token is None:
    raise SystemExit(2)

try:
    width = int(width_token)
    height = int(height_token)
    maxval = int(maxval_token)
except ValueError as exc:
    raise SystemExit(2) from exc

if width <= 0 or height <= 0 or maxval <= 0 or maxval > 255:
    raise SystemExit(2)

if magic == b"P6":
    while index < data_len and data[index] in b" \t\r\n":
        index += 1
    pixel_data = data[index:]
    expected = width * height * 3
    if len(pixel_data) < expected:
        raise SystemExit(2)
    pixel_data = pixel_data[:expected]
    it = iter(pixel_data)
    nonblack = 0
    luma_sum = 0.0
    for r, g, b in zip(it, it, it):
        if r != 0 or g != 0 or b != 0:
            nonblack += 1
        luma_sum += 0.299 * r + 0.587 * g + 0.114 * b
else:
    tokens = re.findall(rb"[0-9]+", data[index:])
    expected = width * height * 3
    if len(tokens) < expected:
        raise SystemExit(2)
    nonblack = 0
    luma_sum = 0.0
    for i in range(0, expected, 3):
        r = int(tokens[i])
        g = int(tokens[i + 1])
        b = int(tokens[i + 2])
        if r != 0 or g != 0 or b != 0:
            nonblack += 1
        luma_sum += 0.299 * r + 0.587 * g + 0.114 * b

total = width * height
nonblack_ratio = float(nonblack) / float(total)
mean_luma = (luma_sum / float(total)) / 255.0

if debug:
    print(
        f"capture content: file={capture_path} nonblack_ratio={nonblack_ratio:.6f} "
        f"mean_luma={mean_luma:.6f} thresholds=[nonblack>={min_nonblack_ratio:.6f}, luma>={min_mean_luma:.6f}]"
    )

if nonblack_ratio >= min_nonblack_ratio and mean_luma >= min_mean_luma:
    raise SystemExit(0)
raise SystemExit(1)
PY
}

capture_via_screenshot() {
  local out_path="$1"
  local scale_div="$2"
  local flip_y="$3"
  local screenshot_dir="$4"

  local before_ns
  before_ns="$(python3 - <<'PY'
import time
print(time.time_ns())
PY
)"

  run_agentctl screenshot >/dev/null

  python3 - "${screenshot_dir}" "${before_ns}" "${scale_div}" "${flip_y}" "${out_path}" <<'PY'
import sys
from pathlib import Path

from PIL import Image

shot_dir = Path(sys.argv[1])
before_ns = int(sys.argv[2])
scale_div = max(1, int(sys.argv[3]))
flip_y = sys.argv[4] == "1"
out_path = Path(sys.argv[5])

if not shot_dir.is_dir():
    raise SystemExit(2)

pngs = [p for p in shot_dir.glob("*.png") if p.is_file()]
if not pngs:
    raise SystemExit(3)

recent = [p for p in pngs if p.stat().st_mtime_ns >= before_ns]
if recent:
    source = max(recent, key=lambda p: p.stat().st_mtime_ns)
else:
    source = max(pngs, key=lambda p: p.stat().st_mtime_ns)

img = Image.open(source).convert("RGB")
if scale_div > 1:
    width, height = img.size
    new_w = max(1, width // scale_div)
    new_h = max(1, height // scale_div)
    resampling = getattr(Image, "Resampling", None)
    nearest = resampling.NEAREST if resampling is not None else Image.NEAREST
    img = img.resize((new_w, new_h), resample=nearest)
if flip_y:
    img = img.transpose(Image.Transpose.FLIP_TOP_BOTTOM)

out_path.parent.mkdir(parents=True, exist_ok=True)
img.save(out_path, format="PPM")
PY
}

mupen_pid=""
launch_log=""
needs_depth_blit_log_checks=0
if [[ "${REQUIRE_NO_DEPTH_BLIT_FAIL}" == "1" || "${REQUIRE_DEPTH_BLIT_STATS}" == "1" || -n "${DEPTH_BLIT_SUMMARY_OUT}" ]]; then
  needs_depth_blit_log_checks=1
fi

cleanup() {
  if [[ -n "${mupen_pid}" ]]; then
    if kill -0 "${mupen_pid}" >/dev/null 2>&1; then
      run_agentctl shutdown >/dev/null 2>&1 || true
      for _ in $(seq 1 40); do
        if ! kill -0 "${mupen_pid}" >/dev/null 2>&1; then
          break
        fi
        sleep 0.1
      done
      if kill -0 "${mupen_pid}" >/dev/null 2>&1; then
        kill "${mupen_pid}" >/dev/null 2>&1 || true
      fi
    fi
  fi
  if [[ -n "${launch_log}" && -z "${REALITYVK_SMOKE_LAUNCH_LOG:-}" ]]; then
    rm -f "${launch_log}" >/dev/null 2>&1 || true
  fi
}
trap cleanup EXIT

rm -f "${SOCKET_PATH}"
if [[ "${REQUIRE_READBACK_MARKER}" == "1" || "${needs_depth_blit_log_checks}" == "1" ]]; then
  launch_log="${REALITYVK_SMOKE_LAUNCH_LOG:-}"
  if [[ -z "${launch_log}" ]]; then
    launch_log="$(mktemp "/tmp/realityvk-smoke-launch.XXXXXX.log")"
  fi
fi

if [[ "${needs_depth_blit_log_checks}" == "1" ]]; then
  if [[ -z "${REALITYVK_VK_TRACE_FBO:-}" ]]; then
    export REALITYVK_VK_TRACE_FBO=1
  fi
  if [[ -z "${REALITYVK_VK_TRACE_FBO_LIMIT:-}" ]]; then
    export REALITYVK_VK_TRACE_FBO_LIMIT=4000
  fi
fi

launch_cmd=(
  "${LAUNCH_SCRIPT}" "${ROM_PATH}"
  --agent-server "${SOCKET_PATH}"
  --agent-profile "${AGENT_PROFILE}"
  --gfx "${backend_plugin_name}"
)
launch_runner=("${launch_cmd[@]}")
if [[ "${LAUNCH_WITH_PTY}" == "1" ]]; then
  if ! command -v script >/dev/null 2>&1; then
    echo "ERROR: REALITYVK_SMOKE_LAUNCH_WITH_PTY=1 requested but 'script' command is unavailable." >&2
    exit 2
  fi
  printf -v launch_cmd_str '%q ' "${launch_cmd[@]}"
  launch_cmd_str="${launch_cmd_str% }"
  launch_runner=(script -qec "${launch_cmd_str}" /dev/null)
fi

if [[ "${LAUNCH_QUIET}" == "1" ]]; then
  if [[ -n "${launch_log}" ]]; then
    "${launch_runner[@]}" >"${launch_log}" 2>&1 &
  else
    "${launch_runner[@]}" >/dev/null 2>&1 &
  fi
else
  if [[ -n "${launch_log}" ]]; then
    "${launch_runner[@]}" > >(tee -a "${launch_log}") 2> >(tee -a "${launch_log}" >&2) &
  else
    "${launch_runner[@]}" &
  fi
fi
mupen_pid="$!"

for _ in $(seq 1 120); do
  if [[ -S "${SOCKET_PATH}" ]]; then
    break
  fi
  if ! kill -0 "${mupen_pid}" >/dev/null 2>&1; then
    wait "${mupen_pid}" || true
    echo "ERROR: mupen process exited before agent socket became ready." >&2
    exit 1
  fi
  sleep 0.1
done

if [[ ! -S "${SOCKET_PATH}" ]]; then
  echo "ERROR: timed out waiting for agent socket: ${SOCKET_PATH}" >&2
  exit 1
fi

run_agentctl --wait-running --wait-timeout 30 status >/dev/null
run_agentctl pause >/dev/null

if [[ -n "${STATE_PATH}" ]]; then
  run_agentctl load "${STATE_PATH}" >/dev/null
  run_agentctl pause >/dev/null
  step_frames "${SETTLE_FRAMES_AFTER_LOAD}"
fi

step_frames "${FRAMES}"

capture_ok=0
capture_attempt=0
while (( capture_attempt <= CAPTURE_RETRY_COUNT )); do
  if [[ "${CAPTURE_METHOD}" == "screenshot" ]]; then
    capture_via_screenshot "${OUT_PATH}" "${SCALE_DIV}" "${SCREENSHOT_FLIP_Y_EFFECTIVE}" "${SCREENSHOT_DIR}" || {
      echo "ERROR: screenshot capture failed: ${OUT_PATH}" >&2
      exit 1
    }
  else
    dumpfb_args=(dumpfb-preset "${OUT_PATH}" "${PRESET}" --scale-div "${SCALE_DIV}")
    if [[ "${DUMPFB_FLIP_Y}" == "1" ]]; then
      dumpfb_args+=(--flip-y)
    fi
    run_agentctl "${dumpfb_args[@]}" >/dev/null
  fi

  if [[ ! -s "${OUT_PATH}" ]]; then
    echo "ERROR: capture output was not produced: ${OUT_PATH}" >&2
    exit 1
  fi

  if [[ "${REQUIRE_NON_BLACK_CAPTURE}" != "1" ]]; then
    capture_ok=1
    break
  fi

  if capture_has_content "${OUT_PATH}"; then
    capture_ok=1
    break
  fi
  content_status=$?
  if [[ "${content_status}" == "2" ]]; then
    echo "ERROR: failed to analyze capture content: ${OUT_PATH}" >&2
    exit 1
  fi

  if (( capture_attempt == CAPTURE_RETRY_COUNT )); then
    break
  fi

  if [[ "${CAPTURE_RETRY_STEP_FRAMES}" == "0" ]]; then
    echo "WARN: capture appears mostly black; retrying without paused stepping." >&2
  else
    echo "WARN: capture appears mostly black; stepping ${CAPTURE_RETRY_STEP_FRAMES} extra paused frames and retrying (${capture_attempt}/${CAPTURE_RETRY_COUNT})." >&2
  fi

  if [[ "${CAPTURE_RETRY_RESUME_MS}" != "0" ]]; then
    echo "WARN: capture retry warmup: resuming for ${CAPTURE_RETRY_RESUME_MS} ms before next capture attempt." >&2
    run_agentctl resume >/dev/null
    sleep_ms "${CAPTURE_RETRY_RESUME_MS}"
    run_agentctl pause >/dev/null
  fi

  if [[ "${CAPTURE_RETRY_STEP_FRAMES}" != "0" ]]; then
    step_frames "${CAPTURE_RETRY_STEP_FRAMES}"
  fi

  capture_attempt="$(( capture_attempt + 1 ))"
done

if [[ "${capture_ok}" != "1" ]]; then
  echo "ERROR: capture remained mostly black after ${CAPTURE_RETRY_COUNT} retries: ${OUT_PATH}" >&2
  exit 1
fi

if [[ "${REQUIRE_READBACK_MARKER}" == "1" ]]; then
  if [[ -z "${launch_log}" || ! -f "${launch_log}" ]]; then
    echo "ERROR: readback marker was required, but launch log is unavailable." >&2
    exit 1
  fi
  if ! grep -qE "${READBACK_MARKER_REGEX}" "${launch_log}"; then
    echo "ERROR: required readback marker not found in launch log." >&2
    echo "       regex: ${READBACK_MARKER_REGEX}" >&2
    echo "       log: ${launch_log}" >&2
    tail -n 120 "${launch_log}" >&2 || true
    exit 1
  fi
fi

if [[ "${REQUIRE_NO_DEPTH_BLIT_FAIL}" == "1" || "${REQUIRE_DEPTH_BLIT_STATS}" == "1" || -n "${DEPTH_BLIT_SUMMARY_OUT}" ]]; then
  if [[ -z "${launch_log}" || ! -f "${launch_log}" ]]; then
    echo "ERROR: depth blit validation was requested, but launch log is unavailable." >&2
    exit 1
  fi
  python3 - "${launch_log}" "${DEPTH_BLIT_FAIL_REGEX}" "${DEPTH_BLIT_SUMMARY_OUT}" "${REQUIRE_NO_DEPTH_BLIT_FAIL}" "${REQUIRE_DEPTH_BLIT_STATS}" <<'PY'
import json
import re
import sys
from pathlib import Path

log_path = Path(sys.argv[1])
fail_regex = sys.argv[2]
summary_out = sys.argv[3]
require_no_fail = sys.argv[4] == "1"
require_stats = sys.argv[5] == "1"

lines = log_path.read_text(encoding="utf-8", errors="replace").splitlines()
fail_re = re.compile(fail_regex)
fail_lines = [line for line in lines if fail_re.search(line)]

reason_re = re.compile(r"\breason=([A-Za-z0-9_]+)")
reason_counts = {}
for line in fail_lines:
    match = reason_re.search(line)
    reason = match.group(1) if match else "unknown"
    reason_counts[reason] = reason_counts.get(reason, 0) + 1

stats_re = re.compile(r"depthStats=\[attempts=(\d+) success=(\d+) fail=(\d+)\]")
stats = None
for line in lines:
    for match in stats_re.finditer(line):
        stats = {
            "attempts": int(match.group(1)),
            "successes": int(match.group(2)),
            "failures": int(match.group(3)),
        }

summary = {
    "log": str(log_path),
    "depth_blit_fail_count": len(fail_lines),
    "depth_blit_fail_reasons": reason_counts,
    "depth_stats_seen": stats is not None,
    "depth_stats": stats,
    "strict_no_depth_fail": require_no_fail,
    "require_depth_stats": require_stats,
}

if summary_out:
    summary_path = Path(summary_out)
    summary_path.parent.mkdir(parents=True, exist_ok=True)
    summary_path.write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"depth blit summary: {summary_path}")

if require_stats and stats is None:
    print("ERROR: required depth blit stats marker was not found in launch log.", file=sys.stderr)
    print(f"       log: {log_path}", file=sys.stderr)
    raise SystemExit(1)

if require_no_fail and fail_lines:
    print("ERROR: depth blit failure markers detected in launch log.", file=sys.stderr)
    print(f"       regex: {fail_regex}", file=sys.stderr)
    print(f"       log: {log_path}", file=sys.stderr)
    for line in fail_lines[:20]:
        print(line, file=sys.stderr)
    raise SystemExit(1)
PY
fi

run_agentctl shutdown >/dev/null || true
wait "${mupen_pid}" || true
mupen_pid=""
