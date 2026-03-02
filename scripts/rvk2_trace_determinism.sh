#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RUNS="${REALITYVK_RVK2_DETERMINISM_RUNS:-2}"
STRICT_MATCH="${REALITYVK_RVK2_DETERMINISM_STRICT_MATCH:-0}"
OUT_DIR="${REALITYVK_RVK2_DETERMINISM_OUT_DIR:-${ROOT_DIR}/build/rvk2-determinism}"

if ! [[ "${RUNS}" =~ ^[0-9]+$ ]] || [[ "${RUNS}" -lt 1 ]]; then
  echo "ERROR: REALITYVK_RVK2_DETERMINISM_RUNS must be an integer >= 1." >&2
  exit 2
fi

mkdir -p "${OUT_DIR}"

fingerprints=()

for ((run_idx=1; run_idx<=RUNS; ++run_idx)); do
  trace_file="${OUT_DIR}/run-${run_idx}.packet.tsv"
  report_file="${OUT_DIR}/run-${run_idx}.replay.json"

  echo "==> [determinism] run ${run_idx}/${RUNS}"
  env \
    REALITYVK_GATE_WITH_SMOKE=1 \
    REALITYVK_PM_VISUAL_GATE=0 \
    REALITYVK_GATE_RVK2_TRACE_FILE="${trace_file}" \
    REALITYVK_GATE_RVK2_TRACE_REPORT_FILE="${report_file}" \
    "${ROOT_DIR}/scripts/local_gate.sh"

  if [[ ! -s "${report_file}" ]]; then
    echo "ERROR: missing replay report: ${report_file}" >&2
    exit 1
  fi

  run_fingerprint="$(
    python3 - "${report_file}" <<'PY'
import hashlib
import json
import sys
from pathlib import Path

report = json.loads(Path(sys.argv[1]).read_text(encoding="utf-8"))
if not report.get("all_ok", False):
    print("ERROR: replay report contains failures", file=sys.stderr)
    raise SystemExit(1)

payload = []
for frame in report.get("frames", []):
    payload.append({
        "frame_id": frame["frame_id"],
        "parsed_command_count": frame["parsed_command_count"],
        "computed_command_hash": frame["computed_command_hash"],
        "computed_state_hash": frame["computed_state_hash"],
    })
blob = json.dumps(payload, sort_keys=True, separators=(",", ":")).encode("utf-8")
print(hashlib.sha256(blob).hexdigest())
PY
  )"

  fingerprints+=("${run_fingerprint}")
  echo "    replay fingerprint: ${run_fingerprint}"
done

base_fingerprint="${fingerprints[0]}"
all_match=1
for fingerprint in "${fingerprints[@]}"; do
  if [[ "${fingerprint}" != "${base_fingerprint}" ]]; then
    all_match=0
    break
  fi
done

summary_file="${OUT_DIR}/summary.json"
python3 - "${summary_file}" "${RUNS}" "${STRICT_MATCH}" "${all_match}" "${fingerprints[@]}" <<'PY'
import json
import sys
from pathlib import Path

summary_path = Path(sys.argv[1])
runs = int(sys.argv[2])
strict_match = sys.argv[3] == "1"
all_match = sys.argv[4] == "1"
fingerprints = list(sys.argv[5:])
summary = {
    "runs": runs,
    "strict_match": strict_match,
    "all_fingerprints_match": all_match,
    "fingerprints": fingerprints,
}
summary_path.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
print(f"determinism summary: {summary_path}")
PY

if [[ "${all_match}" != "1" ]]; then
  if [[ "${STRICT_MATCH}" == "1" ]]; then
    echo "ERROR: determinism fingerprints differ across runs." >&2
    exit 1
  fi
  echo "WARN: determinism fingerprints differ across runs (strict match disabled)." >&2
else
  echo "==> [determinism] fingerprints match across ${RUNS} run(s)"
fi
