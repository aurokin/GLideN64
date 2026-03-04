#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT_DIR}/scripts/lib/validation.sh"
BUILD_ROOT="${ROOT_DIR}/build/local-gate"
JOBS="${REALITYVK_GATE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)}"
WITH_SMOKE="${REALITYVK_GATE_WITH_SMOKE:-0}"
SMOKE_REQUIRE_READBACK_MARKER="${REALITYVK_GATE_SMOKE_REQUIRE_READBACK_MARKER:-1}"
SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL="${REALITYVK_GATE_SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL:-1}"
SMOKE_REQUIRE_DEPTH_BLIT_STATS="${REALITYVK_GATE_SMOKE_REQUIRE_DEPTH_BLIT_STATS:-1}"
SMOKE_CAPTURE_DEPTH_SUMMARY="${REALITYVK_GATE_SMOKE_CAPTURE_DEPTH_SUMMARY:-1}"
SMOKE_DEEP_TELEMETRY="${REALITYVK_GATE_SMOKE_DEEP_TELEMETRY:-0}"
SMOKE_DEEP_TELEMETRY_ROOT="${REALITYVK_GATE_SMOKE_DEEP_TELEMETRY_ROOT:-${BUILD_ROOT}/paper-mario-telemetry}"
SMOKE_DEEP_TELEMETRY_REPLAY_STATEFUL="${REALITYVK_GATE_SMOKE_DEEP_TELEMETRY_REPLAY_STATEFUL:-1}"
SMOKE_DEEP_TELEMETRY_REUSE_REPLAY_REPORT="${REALITYVK_GATE_SMOKE_DEEP_TELEMETRY_REUSE_REPLAY_REPORT:-1}"
WITH_RVK2_TRACE_REPLAY="${REALITYVK_GATE_RVK2_TRACE_REPLAY:-1}"
RVK2_TRACE_REPLAY_STRICT="${REALITYVK_GATE_RVK2_TRACE_REPLAY_STRICT:-1}"
RVK2_TRACE_REPLAY_JOBS="${REALITYVK_GATE_RVK2_TRACE_REPLAY_JOBS:-0}"
RVK2_TRACE_FILE="${REALITYVK_GATE_RVK2_TRACE_FILE:-${BUILD_ROOT}/rvk2.packet.tsv}"
RVK2_TRACE_REPORT_FILE="${REALITYVK_GATE_RVK2_TRACE_REPORT_FILE:-${BUILD_ROOT}/rvk2.packet.replay.json}"
RVK2_FORENSICS_FILE="${REALITYVK_GATE_RVK2_FORENSICS_FILE:-${BUILD_ROOT}/rvk2.frame-forensics.tsv}"
SMOKE_DEEP_TELEMETRY_REPLAY_STRICT="${REALITYVK_GATE_SMOKE_DEEP_TELEMETRY_REPLAY_STRICT:-0}"
SMOKE_DEEP_TELEMETRY_REPLAY_JOBS="${REALITYVK_GATE_SMOKE_DEEP_TELEMETRY_REPLAY_JOBS:-${RVK2_TRACE_REPLAY_JOBS}}"
RUN_RVK2_UNIT_TESTS="${REALITYVK_GATE_RUN_RVK2_UNIT_TESTS:-1}"
RUN_RVK2_CONFORMANCE_TESTS="${REALITYVK_GATE_RUN_RVK2_CONFORMANCE_TESTS:-1}"
RUN_SCRIPT_TESTS="${REALITYVK_GATE_RUN_SCRIPT_TESTS:-1}"
TX_PACK_DIR="${REALITYVK_GATE_TX_PACK_DIR:-}"
TX_PACK_INDEX="${REALITYVK_GATE_TX_PACK_INDEX:-}"
TX_PACK_REQUIRE_COVERAGE="${REALITYVK_GATE_TX_PACK_REQUIRE_COVERAGE:-0}"
TX_PACK_ALLOW_EMPTY="${REALITYVK_GATE_TX_PACK_ALLOW_EMPTY:-0}"
TX_PACK_ALLOW_ABSOLUTE_PATHS="${REALITYVK_GATE_TX_PACK_ALLOW_ABSOLUTE_PATHS:-0}"
TX_PACK_VALIDATE="${REALITYVK_GATE_TX_PACK_VALIDATE:-}"
RUN_SHELLCHECK="${REALITYVK_GATE_RUN_SHELLCHECK:-1}"
if [[ -z "${TX_PACK_VALIDATE}" ]]; then
  if [[ -n "${TX_PACK_DIR}" ]]; then
    TX_PACK_VALIDATE=1
  else
    TX_PACK_VALIDATE=0
  fi
fi

if ! command -v cmake >/dev/null 2>&1; then
  echo "ERROR: cmake is required but was not found in PATH." >&2
  exit 127
fi

rvk2_require_bool "REALITYVK_GATE_WITH_SMOKE" "${WITH_SMOKE}"
rvk2_require_bool "REALITYVK_GATE_SMOKE_REQUIRE_READBACK_MARKER" "${SMOKE_REQUIRE_READBACK_MARKER}"
rvk2_require_bool "REALITYVK_GATE_SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL" "${SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL}"
rvk2_require_bool "REALITYVK_GATE_SMOKE_REQUIRE_DEPTH_BLIT_STATS" "${SMOKE_REQUIRE_DEPTH_BLIT_STATS}"
rvk2_require_bool "REALITYVK_GATE_SMOKE_CAPTURE_DEPTH_SUMMARY" "${SMOKE_CAPTURE_DEPTH_SUMMARY}"
rvk2_require_bool "REALITYVK_GATE_SMOKE_DEEP_TELEMETRY" "${SMOKE_DEEP_TELEMETRY}"
rvk2_require_bool "REALITYVK_GATE_SMOKE_DEEP_TELEMETRY_REPLAY_STATEFUL" "${SMOKE_DEEP_TELEMETRY_REPLAY_STATEFUL}"
rvk2_require_bool "REALITYVK_GATE_SMOKE_DEEP_TELEMETRY_REUSE_REPLAY_REPORT" "${SMOKE_DEEP_TELEMETRY_REUSE_REPLAY_REPORT}"
rvk2_require_bool "REALITYVK_GATE_RVK2_TRACE_REPLAY" "${WITH_RVK2_TRACE_REPLAY}"
rvk2_require_bool "REALITYVK_GATE_RVK2_TRACE_REPLAY_STRICT" "${RVK2_TRACE_REPLAY_STRICT}"
rvk2_require_bool "REALITYVK_GATE_SMOKE_DEEP_TELEMETRY_REPLAY_STRICT" "${SMOKE_DEEP_TELEMETRY_REPLAY_STRICT}"
rvk2_require_bool "REALITYVK_GATE_RUN_RVK2_UNIT_TESTS" "${RUN_RVK2_UNIT_TESTS}"
rvk2_require_bool "REALITYVK_GATE_RUN_RVK2_CONFORMANCE_TESTS" "${RUN_RVK2_CONFORMANCE_TESTS}"
rvk2_require_bool "REALITYVK_GATE_RUN_SCRIPT_TESTS" "${RUN_SCRIPT_TESTS}"
rvk2_require_bool "REALITYVK_GATE_TX_PACK_REQUIRE_COVERAGE" "${TX_PACK_REQUIRE_COVERAGE}"
rvk2_require_bool "REALITYVK_GATE_TX_PACK_ALLOW_EMPTY" "${TX_PACK_ALLOW_EMPTY}"
rvk2_require_bool "REALITYVK_GATE_TX_PACK_ALLOW_ABSOLUTE_PATHS" "${TX_PACK_ALLOW_ABSOLUTE_PATHS}"
rvk2_require_bool "REALITYVK_GATE_TX_PACK_VALIDATE" "${TX_PACK_VALIDATE}"
rvk2_require_bool "REALITYVK_GATE_RUN_SHELLCHECK" "${RUN_SHELLCHECK}"
rvk2_require_uint_ge "REALITYVK_GATE_SMOKE_DEEP_TELEMETRY_REPLAY_JOBS" "${SMOKE_DEEP_TELEMETRY_REPLAY_JOBS}" 0
rvk2_require_uint_ge "REALITYVK_GATE_RVK2_TRACE_REPLAY_JOBS" "${RVK2_TRACE_REPLAY_JOBS}" 0

validate_doc_links() {
  local removed_paths=(
    "WORKFLOW.md"
    "docs/local-smoke.md"
    "docs/n64-runtime-validation-checklist.md"
    "docs/vulkan-core-rebuild-plan.md"
    "docs/references/n64/issue-research/"
  )
  local active_docs=(
    "${ROOT_DIR}/README.md"
    "${ROOT_DIR}/AGENTS.md"
    "${ROOT_DIR}/docs/README.md"
    "${ROOT_DIR}/docs/local-ci.md"
    "${ROOT_DIR}/docs/workflow.md"
    "${ROOT_DIR}/docs/status.md"
  )

  local stale_found=0
  for removed_path in "${removed_paths[@]}"; do
    local matches=""
    matches="$(grep -n -F -- "${removed_path}" "${active_docs[@]}" 2>/dev/null || true)"
    if [[ -n "${matches}" ]]; then
      if [[ "${stale_found}" == "0" ]]; then
        echo "ERROR: stale documentation references detected in active docs/workflow:" >&2
      fi
      echo "${matches}" >&2
      stale_found=1
    fi
  done

  if [[ "${stale_found}" == "1" ]]; then
    exit 1
  fi
}

run_shellcheck() {
  if [[ "${RUN_SHELLCHECK}" != "1" ]]; then
    return
  fi
  if ! command -v shellcheck >/dev/null 2>&1; then
    echo "ERROR: shellcheck is required when REALITYVK_GATE_RUN_SHELLCHECK=1." >&2
    exit 127
  fi

  local -a files=(
    "${ROOT_DIR}/scripts/local_gate.sh"
    "${ROOT_DIR}/scripts/paper_mario_parity.sh"
    "${ROOT_DIR}/scripts/paper_mario_smoke_runner.sh"
    "${ROOT_DIR}/scripts/paper_mario_compare_view.sh"
    "${ROOT_DIR}/scripts/lib/validation.sh"
    "${ROOT_DIR}/scripts/lib/compare_view.sh"
    "${ROOT_DIR}/scripts/lib/paper_mario_parity_env.sh"
  )
  echo "==> [lint] Shellcheck"
  shellcheck "${files[@]}"
  echo "==> [lint] Shellcheck OK"
}

run_script_tests() {
  if [[ "${RUN_SCRIPT_TESTS}" != "1" ]]; then
    return
  fi
  if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 is required when REALITYVK_GATE_RUN_SCRIPT_TESTS=1." >&2
    exit 127
  fi

  echo "==> [tests] Python script/unit tests"
  python3 -m unittest discover -s "${ROOT_DIR}/tests/python" -p "test_*.py" -v
  echo "==> [tests] Python script/unit tests OK"
}

configure_and_build() {
  local name="$1"
  local build_type="$2"
  local build_dir="${BUILD_ROOT}/${name}"

  echo "==> [${name}] Configure"
  cmake -S "${ROOT_DIR}/src" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DMUPENPLUSAPI=ON \
    -DUSE_IPO=OFF

  echo "==> [${name}] Build"
  cmake --build "${build_dir}" --parallel "${JOBS}"

  local artifact_count
  artifact_count="$(find "${build_dir}/plugin" -maxdepth 3 -type f \( -name '*RealityVK*.so' -o -name '*RealityVK*.dll' -o -name '*RealityVK*.dylib' \) | wc -l | tr -d ' ')"
  if [[ "${artifact_count}" == "0" ]]; then
    echo "ERROR: [${name}] no RealityVK plugin artifact was produced." >&2
    exit 1
  fi

  echo "==> [${name}] OK"
}

run_rvk2_unit_tests() {
  local name="$1"
  local build_dir="${BUILD_ROOT}/${name}"
  local test_bin="${build_dir}/rvk2_unit_tests"
  if [[ ! -x "${test_bin}" ]]; then
    test_bin="${build_dir}/rvk2_unit_tests.exe"
  fi
  if [[ ! -x "${test_bin}" ]]; then
    echo "ERROR: [${name}] rvk2_unit_tests binary was not produced." >&2
    exit 1
  fi

  echo "==> [${name}] Run rvk2 unit tests"
  "${test_bin}"
  echo "==> [${name}] rvk2 unit tests OK"
}

run_rvk2_conformance_tests() {
  local name="$1"
  local build_dir="${BUILD_ROOT}/${name}"
  local test_bin="${build_dir}/rvk2_conformance_tests"
  if [[ ! -x "${test_bin}" ]]; then
    test_bin="${build_dir}/rvk2_conformance_tests.exe"
  fi
  if [[ ! -x "${test_bin}" ]]; then
    echo "ERROR: [${name}] rvk2_conformance_tests binary was not produced." >&2
    exit 1
  fi

  echo "==> [${name}] Run rvk2 conformance tests"
  "${test_bin}"
  echo "==> [${name}] rvk2 conformance tests OK"
}

validate_texture_pack_index() {
  if [[ "${TX_PACK_VALIDATE}" != "1" ]]; then
    return
  fi
  if [[ -z "${TX_PACK_DIR}" ]]; then
    echo "ERROR: REALITYVK_GATE_TX_PACK_VALIDATE=1 requires REALITYVK_GATE_TX_PACK_DIR." >&2
    exit 1
  fi
  if [[ ! -d "${TX_PACK_DIR}" ]]; then
    echo "ERROR: REALITYVK_GATE_TX_PACK_DIR does not exist: ${TX_PACK_DIR}" >&2
    exit 1
  fi
  if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 is required for texture-pack index validation." >&2
    exit 127
  fi

  local validate_args=(
    "${ROOT_DIR}/scripts/rvk2_texture_pack_index.py"
    validate
    --pack-dir "${TX_PACK_DIR}"
  )
  if [[ -n "${TX_PACK_INDEX}" ]]; then
    validate_args+=(--index "${TX_PACK_INDEX}")
  fi
  if [[ "${TX_PACK_REQUIRE_COVERAGE}" == "1" ]]; then
    validate_args+=(--require-index-covers-pack)
  fi
  if [[ "${TX_PACK_ALLOW_EMPTY}" == "1" ]]; then
    validate_args+=(--allow-empty)
  fi
  if [[ "${TX_PACK_ALLOW_ABSOLUTE_PATHS}" == "1" ]]; then
    validate_args+=(--allow-absolute-paths)
  fi

  echo "==> [texture-pack] Validate rkv2_pack_index_v1.tsv"
  python3 "${validate_args[@]}"
  echo "==> [texture-pack] OK"
}

mkdir -p "${BUILD_ROOT}"
validate_doc_links
validate_texture_pack_index
run_shellcheck
run_script_tests

configure_and_build "linux-release-cli" "Release"
configure_and_build "linux-debug-cli" "Debug"
if [[ "${RUN_RVK2_UNIT_TESTS}" == "1" ]]; then
  run_rvk2_unit_tests "linux-release-cli"
  run_rvk2_unit_tests "linux-debug-cli"
fi
if [[ "${RUN_RVK2_CONFORMANCE_TESTS}" == "1" ]]; then
  run_rvk2_conformance_tests "linux-release-cli"
  run_rvk2_conformance_tests "linux-debug-cli"
fi

if [[ "${WITH_SMOKE}" == "1" ]]; then
  smoke_name="smoke-release-vulkan"
  configure_and_build "${smoke_name}" "Release"
  if [[ "${RUN_RVK2_UNIT_TESTS}" == "1" ]]; then
    run_rvk2_unit_tests "${smoke_name}"
  fi
  if [[ "${RUN_RVK2_CONFORMANCE_TESTS}" == "1" ]]; then
    run_rvk2_conformance_tests "${smoke_name}"
  fi

  smoke_build_dir="${BUILD_ROOT}/${smoke_name}"
  smoke_plugin_path="$(find "${smoke_build_dir}/plugin" -maxdepth 3 -type f \( -name '*RealityVK*.so' -o -name '*RealityVK*.dll' -o -name '*RealityVK*.dylib' \) | head -n 1)"
  if [[ -z "${smoke_plugin_path}" ]]; then
    echo "ERROR: [${smoke_name}] no plugin artifact found for Vulkan smoke run." >&2
    exit 1
  fi

  echo "==> [smoke] Run Paper Mario visual compare"
  smoke_profile="basic"
  if [[ "${SMOKE_DEEP_TELEMETRY}" == "1" ]]; then
    smoke_profile="deep"
  fi
  smoke_env=(
    REALITYVK_PM_CANDIDATE_PLUGIN="${smoke_plugin_path}"
    REALITYVK_PM_PROFILE="${smoke_profile}"
  )
  if [[ "${SMOKE_DEEP_TELEMETRY}" == "1" ]]; then
    mkdir -p "${SMOKE_DEEP_TELEMETRY_ROOT}"
    RVK2_TRACE_FILE="${SMOKE_DEEP_TELEMETRY_ROOT}/paper_mario_intro.candidate.packet.tsv"
    RVK2_TRACE_REPORT_FILE="${SMOKE_DEEP_TELEMETRY_ROOT}/paper_mario_intro.candidate.packet.replay.json"
    RVK2_FORENSICS_FILE="${SMOKE_DEEP_TELEMETRY_ROOT}/paper_mario_intro.candidate.frame-forensics.tsv"
    smoke_env+=(
      REALITYVK_PM_DEEP_TELEMETRY=1
      REALITYVK_PM_TELEMETRY_ROOT="${SMOKE_DEEP_TELEMETRY_ROOT}"
      REALITYVK_PM_DEEP_TELEMETRY_REPLAY_STRICT="${SMOKE_DEEP_TELEMETRY_REPLAY_STRICT}"
      REALITYVK_PM_DEEP_TELEMETRY_REPLAY_JOBS="${SMOKE_DEEP_TELEMETRY_REPLAY_JOBS}"
      REALITYVK_PM_DEEP_TELEMETRY_REPLAY_STATEFUL="${SMOKE_DEEP_TELEMETRY_REPLAY_STATEFUL}"
    )
  fi
  if [[ "${WITH_RVK2_TRACE_REPLAY}" == "1" ]]; then
    rm -f "${RVK2_TRACE_FILE}" "${RVK2_TRACE_REPORT_FILE}"
    if [[ "${SMOKE_DEEP_TELEMETRY}" != "1" ]]; then
      smoke_env+=(
        REALITYVK2_CAPTURE_RDP_TRACE=1
        REALITYVK2_PACKET_TRACE_FILE="${RVK2_TRACE_FILE}"
      )
    fi
  fi
  if [[ "${SMOKE_REQUIRE_READBACK_MARKER}" == "1" ]]; then
    smoke_env+=(
      REALITYVK_SMOKE_REQUIRE_READBACK_MARKER=1
      REALITYVK_VK_DEBUG_READBACK=1
      REALITYVK_SMOKE_READBACK_MARKER_REGEX='VK readback debug: kind=(pixel|color)'
    )
  fi
  smoke_env+=(
    REALITYVK_PM_REQUIRE_NO_DEPTH_BLIT_FAIL="${SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL}"
    REALITYVK_PM_REQUIRE_DEPTH_BLIT_STATS="${SMOKE_REQUIRE_DEPTH_BLIT_STATS}"
    REALITYVK_PM_CAPTURE_DEPTH_SUMMARY="${SMOKE_CAPTURE_DEPTH_SUMMARY}"
  )
  env "${smoke_env[@]}" "${ROOT_DIR}/scripts/paper_mario_parity.sh"

  if [[ "${WITH_RVK2_TRACE_REPLAY}" == "1" ]]; then
    if [[ ! -s "${RVK2_TRACE_FILE}" ]]; then
      echo "ERROR: [smoke] rvk2 packet trace file was not produced: ${RVK2_TRACE_FILE}" >&2
      exit 1
    fi

    if [[ "${SMOKE_DEEP_TELEMETRY}" == "1" && "${SMOKE_DEEP_TELEMETRY_REUSE_REPLAY_REPORT}" == "1" && -s "${RVK2_TRACE_REPORT_FILE}" ]]; then
      echo "==> [smoke] Reuse deep telemetry replay report"
      python3 - "${RVK2_TRACE_REPORT_FILE}" "${RVK2_TRACE_REPLAY_STRICT}" <<'PY'
import json
import sys
from pathlib import Path

report_path = Path(sys.argv[1])
strict = sys.argv[2] == "1"
report = json.loads(report_path.read_text(encoding="utf-8"))

frame_count = int(report.get("frame_count", 0) or 0)
failed_count = int(report.get("failed_count", 0) or 0)
warning_count = int(report.get("warning_count", 0) or 0)
all_ok = bool(report.get("all_ok", False))

print(
    f"replay report summary: frames={frame_count} failed={failed_count} warned={warning_count} strict={1 if strict else 0}"
)

if frame_count <= 0:
    raise SystemExit("ERROR: replay report contains no frames.")

if failed_count > 0:
    raise SystemExit("ERROR: replay report indicates frame failures.")

if strict and warning_count > 0:
    raise SystemExit("ERROR: strict replay mode rejects warnings present in report.")

if not all_ok:
    raise SystemExit("ERROR: replay report all_ok=false.")
PY
      echo "==> [smoke] rvk2 replay report: ${RVK2_TRACE_REPORT_FILE}"
    else
      echo "==> [smoke] Replay-check rvk2 packet trace"
      replay_args=(
        "${ROOT_DIR}/scripts/rvk2_packet_trace_replay.py"
        --input "${RVK2_TRACE_FILE}"
        --json-out "${RVK2_TRACE_REPORT_FILE}"
        --jobs "${RVK2_TRACE_REPLAY_JOBS}"
      )
      if [[ -s "${RVK2_FORENSICS_FILE}" ]]; then
        replay_args+=(--forensics-file "${RVK2_FORENSICS_FILE}")
      fi
      if [[ "${RVK2_TRACE_REPLAY_STRICT}" == "1" ]]; then
        replay_args+=(--strict)
      fi
      python3 "${replay_args[@]}"
      echo "==> [smoke] rvk2 replay report: ${RVK2_TRACE_REPORT_FILE}"
    fi
  fi
fi

echo "Local gate passed."
