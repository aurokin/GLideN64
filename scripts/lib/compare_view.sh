#!/usr/bin/env bash
# shellcheck shell=bash

rvk2_build_side_by_side_image() {
  local reference_source="$1"
  local candidate_source="$2"
  local out_path="$3"

  if command -v magick >/dev/null 2>&1; then
    magick "${reference_source}" "${candidate_source}" +append "${out_path}"
    return 0
  fi
  if command -v convert >/dev/null 2>&1; then
    convert "${reference_source}" "${candidate_source}" +append "${out_path}"
    return 0
  fi

  echo "ERROR: ImageMagick is required (magick/convert not found)." >&2
  return 1
}

rvk2_close_eog_view() {
  local pid_file="$1"
  local close_all_eog="$2"

  if [[ -f "${pid_file}" ]]; then
    local old_pid
    old_pid="$(cat "${pid_file}" 2>/dev/null || true)"
    if [[ -n "${old_pid}" ]] && kill -0 "${old_pid}" >/dev/null 2>&1; then
      kill "${old_pid}" >/dev/null 2>&1 || true
      sleep 0.15
    fi
    rm -f "${pid_file}"
  fi

  if [[ "${close_all_eog}" == "1" ]]; then
    pkill -x eog >/dev/null 2>&1 || true
  fi
}

rvk2_open_eog_view() {
  local image_path="$1"
  local pid_file="$2"

  if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo "WARN: auto compare viewer skipped (DISPLAY/WAYLAND_DISPLAY not set)." >&2
    return 0
  fi

  if ! command -v eog >/dev/null 2>&1; then
    echo "WARN: auto compare viewer requested but eog is unavailable." >&2
    return 0
  fi

  setsid eog --new-instance "${image_path}" >/dev/null 2>&1 < /dev/null &
  local new_pid="$!"
  disown || true
  sleep 0.2
  if kill -0 "${new_pid}" >/dev/null 2>&1; then
    echo "${new_pid}" > "${pid_file}"
    return 0
  fi

  echo "WARN: eog exited before image view stabilized: ${image_path}" >&2
  rm -f "${pid_file}"
  return 0
}
