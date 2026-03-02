#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUN_ROOT="${REALITYVK_PM_RUN_ROOT:-${ROOT_DIR}/build/parity-runs/paper-mario}"
CACHE_ROOT="${REALITYVK_PM_CACHE_ROOT:-${ROOT_DIR}/build/parity-cache/paper-mario}"
SCENARIO_ID="${REALITYVK_PM_SCENARIO_ID:-paper_mario_intro}"
VIEW_MODE="${REALITYVK_PM_COMPARE_VIEW_MODE:-triptych}" # triptych
REFRESH_REFERENCE="${REALITYVK_PM_REFRESH_REFERENCE:-0}"
VIEWER="${REALITYVK_PM_COMPARE_VIEWER:-eog}"
VISUAL_GATE="${REALITYVK_PM_COMPARE_VISUAL_GATE:-0}"
DUMPFB_FLIP_Y="${REALITYVK_PM_DUMPFB_FLIP_Y:-1}"

CAPTURE_METHOD_TAG="dumpfb_flip${DUMPFB_FLIP_Y}"

REFERENCE_CAPTURE="${CACHE_ROOT}/${SCENARIO_ID}.${CAPTURE_METHOD_TAG}.reference.ppm"
CANDIDATE_CAPTURE="${RUN_ROOT}/${SCENARIO_ID}.candidate.ppm"
DIFF_CAPTURE="${RUN_ROOT}/${SCENARIO_ID}.diff.png"
METRICS_JSON="${RUN_ROOT}/${SCENARIO_ID}.metrics.json"
CAPTURE_CONTEXT_JSON="${RUN_ROOT}/${SCENARIO_ID}.capture-context.json"
REFERENCE_PNG="${RUN_ROOT}/${SCENARIO_ID}.reference.png"
CANDIDATE_PNG="${RUN_ROOT}/${SCENARIO_ID}.candidate.png"

COMPARE_OUT="${RUN_ROOT}/${SCENARIO_ID}.compare_${VIEW_MODE}.latest.png"
VIEWER_PID_FILE="${RUN_ROOT}/.paper_mario_compare_view.pid"

mkdir -p "${RUN_ROOT}" "${CACHE_ROOT}"

if [[ "${VIEW_MODE}" != "triptych" ]]; then
  echo "ERROR: REALITYVK_PM_COMPARE_VIEW_MODE must be 'triptych'." >&2
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

for required in "${REFERENCE_SOURCE}" "${CANDIDATE_SOURCE}" "${DIFF_CAPTURE}"; do
  if [[ ! -s "${required}" ]]; then
    echo "ERROR: missing required compare artifact: ${required}" >&2
    exit 1
  fi
done

echo "==> [compare-view] build composed image (${VIEW_MODE})"
python3 - "${VIEW_MODE}" "${REFERENCE_SOURCE}" "${CANDIDATE_SOURCE}" "${DIFF_CAPTURE}" "${METRICS_JSON}" "${COMPARE_OUT}" <<'PY'
import json
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

view_mode = sys.argv[1]
reference_path = Path(sys.argv[2])
candidate_path = Path(sys.argv[3])
diff_path = Path(sys.argv[4])
metrics_path = Path(sys.argv[5])
out_path = Path(sys.argv[6])

base_h = 0
panels = []

def load_panel(path: Path, label: str):
    img = Image.open(path).convert("RGB")
    return {"label": label, "img": img}

panels.append(load_panel(reference_path, "Reference (maintained capture)"))
panels.append(load_panel(candidate_path, "Candidate (RealityVK Vulkan)"))
panels.append(load_panel(diff_path, "Absolute Difference"))

for panel in panels:
    img = panel["img"]
    if img is not None:
        base_h = max(base_h, img.height)
if base_h == 0:
    raise SystemExit("no readable images found for compare view")

for panel in panels:
    img = panel["img"]
    if img is None:
        panel["size"] = (320, base_h)
        continue
    if img.height != base_h:
        w = int(round(img.width * (base_h / img.height)))
        img = img.resize((max(1, w), base_h), Image.Resampling.NEAREST)
        panel["img"] = img
    panel["size"] = img.size

gap = 20
pad_x = 24
pad_y = 16
label_h = 28
footer_h = 34

total_w = pad_x * 2 + gap * (len(panels) - 1) + sum(panel["size"][0] for panel in panels)
total_h = pad_y * 2 + label_h + base_h + footer_h

canvas = Image.new("RGB", (total_w, total_h), (20, 24, 32))
draw = ImageDraw.Draw(canvas)
font = ImageFont.load_default()

x = pad_x
y_label = pad_y
y_img = pad_y + label_h

for panel in panels:
    w, h = panel["size"]
    draw.rectangle((x - 2, y_img - 2, x + w + 1, y_img + h + 1), outline=(90, 110, 140), width=2)
    draw.text((x, y_label), panel["label"], fill=(235, 240, 250), font=font)
    if panel["img"] is not None:
        canvas.paste(panel["img"], (x, y_img))
    else:
        draw.rectangle((x, y_img, x + w, y_img + h), fill=(28, 34, 46))
        draw.text((x + 8, y_img + 8), "missing", fill=(200, 120, 120), font=font)
    x += w + gap

metrics_line = "metrics unavailable"
if metrics_path.exists():
    try:
        metrics = json.loads(metrics_path.read_text(encoding="utf-8"))
        rmse = metrics.get("rmse")
        mae = metrics.get("mae")
        max_abs = metrics.get("max_abs_diff")
        metrics_line = f"RMSE={rmse:.6f}  MAE={mae:.6f}  MaxAbs={max_abs:.6f}"
    except Exception:
        metrics_line = "metrics parse error"

draw.text((pad_x, total_h - pad_y - 14), metrics_line, fill=(205, 215, 230), font=font)
out_path.parent.mkdir(parents=True, exist_ok=True)
canvas.save(out_path)
print(out_path)
PY

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

  if [[ "${VIEWER}" == "display" ]]; then
    pkill -f "^display .*paper_mario_intro\\.compare_" >/dev/null 2>&1 || true
  fi
  if [[ "${VIEWER}" == "eog" ]]; then
    pkill -f "^eog .*paper_mario_intro\\.compare_" >/dev/null 2>&1 || true
  fi
}

open_viewer() {
  if ! command -v "${VIEWER}" >/dev/null 2>&1; then
    if command -v eog >/dev/null 2>&1; then
      VIEWER="eog"
    elif command -v display >/dev/null 2>&1; then
      VIEWER="display"
    elif command -v xdg-open >/dev/null 2>&1; then
      VIEWER="xdg-open"
    else
      echo "WARN: viewer '${VIEWER}' not found; created image at ${new_view}" >&2
      return
    fi
  fi

  launch_with() {
    local _viewer="$1"
    case "${_viewer}" in
      eog)
        setsid eog --new-instance "${new_view}" >/dev/null 2>&1 < /dev/null &
        ;;
      display)
        setsid display "${new_view}" >/dev/null 2>&1 < /dev/null &
        ;;
      xdg-open)
        setsid xdg-open "${new_view}" >/dev/null 2>&1 < /dev/null &
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

  for candidate_viewer in eog display xdg-open; do
    if [[ "${candidate_viewer}" == "${VIEWER}" ]]; then
      continue
    fi
    if ! command -v "${candidate_viewer}" >/dev/null 2>&1; then
      continue
    fi
    VIEWER="${candidate_viewer}"
    if launch_with "${VIEWER}"; then
      return
    fi
  done

  rm -f "${VIEWER_PID_FILE}"
  echo "WARN: could not keep a viewer process alive; image is at ${new_view}" >&2
}

echo "==> [compare-view] close previous image and open new comparison"
close_old_viewer
open_viewer

echo "compare image: ${new_view}"
echo "reference: ${REFERENCE_SOURCE}"
echo "candidate: ${CANDIDATE_SOURCE}"
echo "diff: ${DIFF_CAPTURE}"
echo "metrics: ${METRICS_JSON}"
if [[ -s "${CAPTURE_CONTEXT_JSON}" ]]; then
  echo "capture context: ${CAPTURE_CONTEXT_JSON}"
fi
