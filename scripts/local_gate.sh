#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_ROOT="${ROOT_DIR}/build/local-gate"
JOBS="${GLIDEN64_GATE_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || nproc 2>/dev/null || echo 4)}"
BACKEND="${GLIDEN64_GRAPHICS_BACKEND:-OpenGL}"
WITH_QT="${GLIDEN64_GATE_WITH_QT:-0}"

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
    -DGLIDEN64_GRAPHICS_BACKEND="${BACKEND}" \
    -DUSE_IPO=OFF

  echo "==> [${name}] Build"
  cmake --build "${build_dir}" --parallel "${JOBS}"

  local artifact_count
  artifact_count="$(find "${build_dir}/plugin" -maxdepth 3 -type f \( -name '*GLideN64*.so' -o -name '*GLideN64*.dll' -o -name '*GLideN64*.dylib' \) | wc -l | tr -d ' ')"
  if [[ "${artifact_count}" == "0" ]]; then
    echo "ERROR: [${name}] no GLideN64 plugin artifact was produced." >&2
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

echo "Local gate passed."
