#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${ROOT_DIR}/build/local-gate"
JOBS="${REALITYVK_GATE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)}"
WITH_QT="${REALITYVK_GATE_WITH_QT:-0}"
WITH_SMOKE="${REALITYVK_GATE_WITH_SMOKE:-0}"
SMOKE_REQUIRE_READBACK_MARKER="${REALITYVK_GATE_SMOKE_REQUIRE_READBACK_MARKER:-1}"
SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL="${REALITYVK_GATE_SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL:-1}"
SMOKE_REQUIRE_DEPTH_BLIT_STATS="${REALITYVK_GATE_SMOKE_REQUIRE_DEPTH_BLIT_STATS:-1}"
SMOKE_CAPTURE_DEPTH_SUMMARY="${REALITYVK_GATE_SMOKE_CAPTURE_DEPTH_SUMMARY:-1}"
WITH_RVK2_TRACE_REPLAY="${REALITYVK_GATE_RVK2_TRACE_REPLAY:-1}"
RVK2_TRACE_REPLAY_STRICT="${REALITYVK_GATE_RVK2_TRACE_REPLAY_STRICT:-0}"
RVK2_TRACE_REPLAY_JOBS="${REALITYVK_GATE_RVK2_TRACE_REPLAY_JOBS:-0}"
RVK2_TRACE_FILE="${REALITYVK_GATE_RVK2_TRACE_FILE:-${BUILD_ROOT}/rvk2.packet.tsv}"
RVK2_TRACE_REPORT_FILE="${REALITYVK_GATE_RVK2_TRACE_REPORT_FILE:-${BUILD_ROOT}/rvk2.packet.replay.json}"
RUN_RVK2_UNIT_TESTS="${REALITYVK_GATE_RUN_RVK2_UNIT_TESTS:-1}"

if ! command -v cmake >/dev/null 2>&1; then
  echo "ERROR: cmake is required but was not found in PATH." >&2
  exit 127
fi

validate_doc_links() {
  local removed_paths=(
    "docs/realityvk-vs-upstream-flow.md"
    "docs/vulkan-migration-plan.md"
    "docs/vulkan-backend-intent.md"
    "docs/n64-video-core-bug-matrix.md"
    "docs/vulkan-core-future-map.md"
    "docs/vulkan-core-owner-map.md"
    "docs/vulkan-core-phase-a-checklist.md"
    "docs/archive/"
  )
  local active_docs=(
    "${ROOT_DIR}/README.md"
    "${ROOT_DIR}/WORKFLOW.md"
    "${ROOT_DIR}/docs/README.md"
    "${ROOT_DIR}/docs/local-ci.md"
    "${ROOT_DIR}/docs/local-smoke.md"
    "${ROOT_DIR}/docs/n64-runtime-validation-checklist.md"
    "${ROOT_DIR}/docs/vulkan-core-rebuild-plan.md"
    "${ROOT_DIR}/docs/vulkan-core-status.md"
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

configure_and_build() {
  local name="$1"
  local build_type="$2"
  local with_ui="$3"
  local build_dir="${BUILD_ROOT}/${name}"

  echo "==> [${name}] Configure"
  cmake -S "${ROOT_DIR}/src" -B "${build_dir}" \
    -DCMAKE_BUILD_TYPE="${build_type}" \
    -DMUPENPLUSAPI=ON \
    -DMUPENPLUSAPI_GLIDENUI="${with_ui}" \
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

mkdir -p "${BUILD_ROOT}"
validate_doc_links

configure_and_build "linux-release-cli" "Release" "OFF"
configure_and_build "linux-debug-cli" "Debug" "OFF"
if [[ "${RUN_RVK2_UNIT_TESTS}" == "1" ]]; then
  run_rvk2_unit_tests "linux-release-cli"
  run_rvk2_unit_tests "linux-debug-cli"
fi

if [[ "${WITH_QT}" == "1" ]]; then
  configure_and_build "linux-release-qt" "Release" "ON"
fi

if [[ "${WITH_SMOKE}" == "1" ]]; then
  smoke_name="smoke-release-vulkan"
  configure_and_build "${smoke_name}" "Release" "OFF"
  if [[ "${RUN_RVK2_UNIT_TESTS}" == "1" ]]; then
    run_rvk2_unit_tests "${smoke_name}"
  fi

  smoke_build_dir="${BUILD_ROOT}/${smoke_name}"
  smoke_plugin_path="$(find "${smoke_build_dir}/plugin" -maxdepth 3 -type f \( -name '*RealityVK*.so' -o -name '*RealityVK*.dll' -o -name '*RealityVK*.dylib' \) | head -n 1)"
  if [[ -z "${smoke_plugin_path}" ]]; then
    echo "ERROR: [${smoke_name}] no plugin artifact found for Vulkan smoke run." >&2
    exit 1
  fi

  reference_plugin="${REALITYVK_PM_REFERENCE_PLUGIN:-/home/auro/code/realityvk-upstream/build-release/plugin/Release/mupen64plus-video-RealityVK.so}"
  echo "==> [smoke] Run Paper Mario visual compare (reference vs candidate)"
  smoke_env=(
    REALITYVK_PM_REFERENCE_PLUGIN="${reference_plugin}"
    REALITYVK_PM_CANDIDATE_PLUGIN="${smoke_plugin_path}"
  )
  if [[ "${WITH_RVK2_TRACE_REPLAY}" == "1" ]]; then
    rm -f "${RVK2_TRACE_FILE}" "${RVK2_TRACE_REPORT_FILE}"
    smoke_env+=(
      REALITYVK2_CAPTURE_RDP_TRACE=1
      REALITYVK2_PACKET_TRACE_FILE="${RVK2_TRACE_FILE}"
    )
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

    echo "==> [smoke] Replay-check rvk2 packet trace"
    replay_args=(
      "${ROOT_DIR}/scripts/rvk2_packet_trace_replay.py"
      --input "${RVK2_TRACE_FILE}"
      --json-out "${RVK2_TRACE_REPORT_FILE}"
      --jobs "${RVK2_TRACE_REPLAY_JOBS}"
    )
    if [[ "${RVK2_TRACE_REPLAY_STRICT}" == "1" ]]; then
      replay_args+=(--strict)
    fi
    python3 "${replay_args[@]}"
    echo "==> [smoke] rvk2 replay report: ${RVK2_TRACE_REPORT_FILE}"
  fi
fi

echo "Local gate passed."
