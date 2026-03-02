#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="${REALITYVK_SMOKE_MANIFEST:-${ROOT_DIR}/tests/smoke/scenarios.tsv}"
OUTPUT_ROOT="${REALITYVK_SMOKE_OUTPUT:-${ROOT_DIR}/build/smoke}"
BASELINE_ROOT="${REALITYVK_SMOKE_BASELINES:-${ROOT_DIR}/tests/smoke/baselines}"
BACKEND="Vulkan"
DEFAULT_RUNNER_TEMPLATE="${ROOT_DIR}/scripts/paper_mario_smoke_runner.sh --backend ${BACKEND} --rom {rom} --frames {frames} --out {out_file} {scenario_args}"
RUNNER_TEMPLATE="${REALITYVK_SMOKE_RUNNER:-${DEFAULT_RUNNER_TEMPLATE}}"
UPDATE_BASELINES="${REALITYVK_SMOKE_UPDATE_BASELINES:-0}"
STRICT_BASELINE="${REALITYVK_SMOKE_STRICT_BASELINE:-1}"
RUNNER_TIMEOUT_SEC="${REALITYVK_SMOKE_TIMEOUT_SEC:-180}"
REPEAT_COUNT="${REALITYVK_SMOKE_REPEAT_COUNT:-1}"
REQUIRE_STABLE_CAPTURE="${REALITYVK_SMOKE_REQUIRE_STABLE_CAPTURE:-1}"
REQUIRE_MAJORITY="${REALITYVK_SMOKE_REQUIRE_MAJORITY:-1}"
REQUESTED_BACKENDS="${REALITYVK_SMOKE_BACKENDS:-Vulkan}"

if [[ ! -f "${MANIFEST}" ]]; then
  echo "ERROR: smoke manifest not found: ${MANIFEST}" >&2
  exit 2
fi

if ! command -v bash >/dev/null 2>&1; then
  echo "ERROR: bash is required to run smoke runner commands." >&2
  exit 127
fi

if ! [[ "${REPEAT_COUNT}" =~ ^[0-9]+$ ]] || [[ "${REPEAT_COUNT}" == "0" ]]; then
  echo "ERROR: REALITYVK_SMOKE_REPEAT_COUNT must be an integer >= 1." >&2
  exit 2
fi

if [[ "${REQUIRE_STABLE_CAPTURE}" != "0" && "${REQUIRE_STABLE_CAPTURE}" != "1" ]]; then
  echo "ERROR: REALITYVK_SMOKE_REQUIRE_STABLE_CAPTURE must be 0 or 1." >&2
  exit 2
fi

if [[ "${REQUIRE_MAJORITY}" != "0" && "${REQUIRE_MAJORITY}" != "1" ]]; then
  echo "ERROR: REALITYVK_SMOKE_REQUIRE_MAJORITY must be 0 or 1." >&2
  exit 2
fi

normalized_backends="$(echo "${REQUESTED_BACKENDS//,/ }" | xargs)"
if [[ "${normalized_backends}" != "Vulkan" ]]; then
  echo "ERROR: Vulkan-only smoke runner supports REALITYVK_SMOKE_BACKENDS=Vulkan only." >&2
  exit 2
fi

hash_file() {
  local file="$1"
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "${file}" | awk '{print $1}'
    return
  fi
  if command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "${file}" | awk '{print $1}'
    return
  fi
  echo "ERROR: sha256sum or shasum is required for smoke checksumming." >&2
  return 127
}

run_with_timeout() {
  local command="$1"
  if command -v timeout >/dev/null 2>&1; then
    timeout "${RUNNER_TIMEOUT_SEC}" bash -lc "${command}"
    return
  fi
  bash -lc "${command}"
}

shell_quote() {
  printf '%q' "$1"
}

build_runner_command() {
  local scenario="$1"
  local rom="$2"
  local frames="$3"
  local out_file="$4"
  local scenario_args="$5"
  local out_dir="$6"

  local command="${RUNNER_TEMPLATE}"
  command="${command//\{scenario\}/$(shell_quote "${scenario}")}"
  command="${command//\{rom\}/$(shell_quote "${rom}")}"
  command="${command//\{frames\}/$(shell_quote "${frames}")}"
  command="${command//\{out_file\}/$(shell_quote "${out_file}")}"
  command="${command//\{out_dir\}/$(shell_quote "${out_dir}")}"
  command="${command//\{scenario_args\}/${scenario_args}}"
  printf '%s' "${command}"
}

declare -a SCENARIOS=()
declare -A SCENARIO_ROMS=()
declare -A SCENARIO_FRAMES=()
declare -A SCENARIO_ARGS=()

while IFS=$'\t' read -r scenario rom frames args || [[ -n "${scenario}${rom}${frames}${args}" ]]; do
  scenario="$(echo "${scenario}" | xargs)"
  if [[ -z "${scenario}" ]]; then
    continue
  fi
  if [[ "${scenario:0:1}" == "#" ]]; then
    continue
  fi
  if [[ -z "${rom}" ]]; then
    echo "ERROR: scenario '${scenario}' has empty rom column." >&2
    exit 2
  fi
  if [[ -z "${frames}" ]]; then
    frames="0"
  fi
  if ! [[ "${frames}" =~ ^[0-9]+$ ]]; then
    echo "ERROR: scenario '${scenario}' has invalid frames value '${frames}'." >&2
    exit 2
  fi
  SCENARIOS+=("${scenario}")
  SCENARIO_ROMS["${scenario}"]="${rom}"
  SCENARIO_FRAMES["${scenario}"]="${frames}"
  SCENARIO_ARGS["${scenario}"]="${args:-}"
done < "${MANIFEST}"

if [[ "${#SCENARIOS[@]}" -eq 0 ]]; then
  echo "ERROR: no active smoke scenarios found in ${MANIFEST}." >&2
  echo "Add at least one non-comment scenario row before enabling smoke gate." >&2
  exit 2
fi

backend_dir="${OUTPUT_ROOT}/${BACKEND}"
mkdir -p "${backend_dir}"
tmp_checksums="${backend_dir}/checksums.tsv.tmp"
: > "${tmp_checksums}"

for scenario in "${SCENARIOS[@]}"; do
  rom="${SCENARIO_ROMS[${scenario}]}"
  frames="${SCENARIO_FRAMES[${scenario}]}"
  scenario_args="${SCENARIO_ARGS[${scenario}]}"
  out_file="${backend_dir}/${scenario}.ppm"
  rm -f "${out_file}" "${backend_dir}/${scenario}.run"*.ppm

  declare -A run_hash_counts=()
  declare -A run_hash_files=()

  for run_idx in $(seq 1 "${REPEAT_COUNT}"); do
    run_out_file="${out_file}"
    if [[ "${REPEAT_COUNT}" != "1" ]]; then
      run_out_file="${backend_dir}/${scenario}.run${run_idx}.ppm"
    fi

    runner_command="$(build_runner_command "${scenario}" "${rom}" "${frames}" "${run_out_file}" "${scenario_args}" "${backend_dir}")"
    if [[ "${REPEAT_COUNT}" == "1" ]]; then
      echo "==> [smoke:${BACKEND}] ${scenario}"
    else
      echo "==> [smoke:${BACKEND}] ${scenario} (run ${run_idx}/${REPEAT_COUNT})"
    fi
    REALITYVK_GRAPHICS_BACKEND="${BACKEND}" run_with_timeout "${runner_command}"

    if [[ ! -s "${run_out_file}" ]]; then
      echo "ERROR: smoke runner did not create output for ${scenario}: ${run_out_file}" >&2
      exit 1
    fi

    run_checksum="$(hash_file "${run_out_file}")"
    run_hash_counts["${run_checksum}"]="$(( ${run_hash_counts[${run_checksum}]:-0} + 1 ))"
    if [[ -z "${run_hash_files[${run_checksum}]:-}" ]]; then
      run_hash_files["${run_checksum}"]="${run_out_file}"
    fi
  done

  selected_checksum=""
  selected_count=0
  for checksum_key in "${!run_hash_counts[@]}"; do
    count="${run_hash_counts[${checksum_key}]}"
    if (( count > selected_count )); then
      selected_count="${count}"
      selected_checksum="${checksum_key}"
    elif (( count == selected_count )) && [[ -n "${selected_checksum}" ]] && [[ "${checksum_key}" < "${selected_checksum}" ]]; then
      selected_checksum="${checksum_key}"
    fi
  done

  if [[ -z "${selected_checksum}" ]]; then
    echo "ERROR: failed to compute checksum for ${scenario}." >&2
    exit 1
  fi

  selected_file="${run_hash_files[${selected_checksum}]}"
  if [[ "${selected_file}" != "${out_file}" ]]; then
    cp "${selected_file}" "${out_file}"
  fi

  if (( ${#run_hash_counts[@]} > 1 )); then
    instability_detail=""
    for checksum_key in "${!run_hash_counts[@]}"; do
      if [[ -n "${instability_detail}" ]]; then
        instability_detail+=", "
      fi
      instability_detail+="${checksum_key}:${run_hash_counts[${checksum_key}]}"
    done
    message="capture instability for ${scenario} over ${REPEAT_COUNT} runs [${instability_detail}]"
    if [[ "${REQUIRE_STABLE_CAPTURE}" == "1" ]]; then
      echo "ERROR: ${message}" >&2
      exit 1
    fi
    if [[ "${REQUIRE_MAJORITY}" == "1" ]] && (( selected_count * 2 <= REPEAT_COUNT )); then
      echo "ERROR: no majority hash for ${scenario} over ${REPEAT_COUNT} runs [${instability_detail}]" >&2
      exit 1
    fi
    echo "WARN: ${message}" >&2
  fi

  printf '%s\t%s\t%s\n' "${scenario}" "${selected_checksum}" "$(basename "${out_file}")" >> "${tmp_checksums}"
done

checksums_file="${backend_dir}/checksums.tsv"
sort -k1,1 "${tmp_checksums}" > "${checksums_file}"
rm -f "${tmp_checksums}"

if [[ "${UPDATE_BASELINES}" == "1" ]]; then
  mkdir -p "${BASELINE_ROOT}"
  cp "${checksums_file}" "${BASELINE_ROOT}/${BACKEND}.checksums.tsv"
  echo "Updated baseline: ${BASELINE_ROOT}/${BACKEND}.checksums.tsv"
  echo "Smoke baseline update complete."
  exit 0
fi

baseline_file="${BASELINE_ROOT}/${BACKEND}.checksums.tsv"
if [[ ! -f "${baseline_file}" ]]; then
  if [[ "${STRICT_BASELINE}" == "1" ]]; then
    echo "ERROR: missing smoke baseline: ${baseline_file}" >&2
    exit 1
  fi
  echo "WARN: missing smoke baseline, skipping baseline diff." >&2
else
  if ! diff -u "${baseline_file}" "${checksums_file}"; then
    echo "ERROR: smoke checksum mismatch for backend '${BACKEND}'." >&2
    exit 1
  fi
fi

echo "Smoke gate passed."
