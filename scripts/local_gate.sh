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

if ! command -v cmake >/dev/null 2>&1; then
  echo "ERROR: cmake is required but was not found in PATH." >&2
  exit 127
fi

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

mkdir -p "${BUILD_ROOT}"

configure_and_build "linux-release-cli" "Release" "OFF"
configure_and_build "linux-debug-cli" "Debug" "OFF"

if [[ "${WITH_QT}" == "1" ]]; then
  configure_and_build "linux-release-qt" "Release" "ON"
fi

if [[ "${WITH_SMOKE}" == "1" ]]; then
  smoke_name="smoke-release-vulkan"
  configure_and_build "${smoke_name}" "Release" "OFF"

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
fi

echo "Local gate passed."
