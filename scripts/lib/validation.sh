#!/usr/bin/env bash
# shellcheck shell=bash

rvk2_die_usage() {
  echo "ERROR: $*" >&2
  exit 2
}

rvk2_require_bool() {
  local name="$1"
  local value="$2"
  if [[ "${value}" != "0" && "${value}" != "1" ]]; then
    rvk2_die_usage "${name} must be 0 or 1."
  fi
}

rvk2_require_uint_ge() {
  local name="$1"
  local value="$2"
  local min="$3"
  if ! [[ "${value}" =~ ^[0-9]+$ ]]; then
    rvk2_die_usage "${name} must be an integer >= ${min}."
  fi
  if (( value < min )); then
    rvk2_die_usage "${name} must be an integer >= ${min}."
  fi
}

rvk2_require_float() {
  local name="$1"
  local value="$2"
  if ! [[ "${value}" =~ ^[0-9]*\.?[0-9]+$ ]]; then
    rvk2_die_usage "${name} must be numeric."
  fi
}

rvk2_require_enum() {
  local name="$1"
  local value="$2"
  shift 2
  local option
  for option in "$@"; do
    if [[ "${value}" == "${option}" ]]; then
      return 0
    fi
  done

  local joined=""
  for option in "$@"; do
    if [[ -z "${joined}" ]]; then
      joined="'${option}'"
    else
      joined="${joined}, '${option}'"
    fi
  done
  rvk2_die_usage "${name} must be one of: ${joined}."
}

rvk2_require_file() {
  local name="$1"
  local path="$2"
  if [[ ! -f "${path}" ]]; then
    rvk2_die_usage "${name} not found: ${path}"
  fi
}

rvk2_require_executable() {
  local name="$1"
  local path="$2"
  if [[ ! -x "${path}" ]]; then
    rvk2_die_usage "${name} not executable: ${path}"
  fi
}
