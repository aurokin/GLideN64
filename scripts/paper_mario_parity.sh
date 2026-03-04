#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT_DIR}/scripts/lib/validation.sh"
source "${ROOT_DIR}/scripts/lib/compare_view.sh"
source "${ROOT_DIR}/scripts/lib/paper_mario_parity_env.sh"
MANIFEST="${REALITYVK_PM_MANIFEST:-${ROOT_DIR}/tests/smoke/scenarios.tsv}"
SCENARIO_ID="${REALITYVK_PM_SCENARIO_ID:-paper_mario_intro}"

CANDIDATE_PLUGIN="${REALITYVK_PM_CANDIDATE_PLUGIN:-${ROOT_DIR}/build/release-vulkan-smoke/plugin/Release/mupen64plus-video-RealityVK.so}"
CANDIDATE_CORELIB="${REALITYVK_PM_CANDIDATE_CORELIB:-/home/auro/code/mupen/mupen64plus-core/projects/unix/libmupen64plus.so.2}"
REFERENCE_PLUGIN="${REALITYVK_PM_REFERENCE_PLUGIN:-/home/auro/code/gliden64-upstream/build-release/plugin/Release/mupen64plus-video-GLideN64.so}"
REFERENCE_CORELIB="${REALITYVK_PM_REFERENCE_CORELIB:-/home/auro/code/mupen/mupen64plus-core-upstream/projects/unix/libmupen64plus.so.2}"

CACHE_ROOT="${REALITYVK_PM_CACHE_ROOT:-${ROOT_DIR}/build/parity-cache/paper-mario}"
RUN_ROOT="${REALITYVK_PM_RUN_ROOT:-${ROOT_DIR}/build/parity-runs/paper-mario}"
PROFILE="${REALITYVK_PM_PROFILE:-basic}"
KNOB_TRACK_ENABLE="${REALITYVK_PM_KNOB_TRACK_ENABLE:-1}"
KNOB_TRACK_WINDOW="${REALITYVK_PM_KNOB_TRACK_WINDOW:-20}"
KNOB_TRACK_SUMMARY_LIMIT="${REALITYVK_PM_KNOB_TRACK_SUMMARY_LIMIT:-12}"
KNOB_TRACK_FILE="${REALITYVK_PM_KNOB_TRACK_FILE:-${RUN_ROOT}/knob-history.tsv}"
DRY_RUN="${REALITYVK_PM_DRY_RUN:-0}"
DRY_RUN_OUT="${REALITYVK_PM_DRY_RUN_OUT:-${RUN_ROOT}/${SCENARIO_ID}.dry-run.json}"

REFRESH_REFERENCE="${REALITYVK_PM_REFRESH_REFERENCE:-0}"
VISUAL_GATE="${REALITYVK_PM_VISUAL_GATE:-1}"
RMSE_MAX="${REALITYVK_PM_VISUAL_RMSE_MAX:-0.25}"
MAE_MAX="${REALITYVK_PM_VISUAL_MAE_MAX:-}"
CAPTURE_DEPTH_SUMMARY="${REALITYVK_PM_CAPTURE_DEPTH_SUMMARY:-1}"
REQUIRE_NO_DEPTH_BLIT_FAIL="${REALITYVK_PM_REQUIRE_NO_DEPTH_BLIT_FAIL:-0}"
REQUIRE_DEPTH_BLIT_STATS="${REALITYVK_PM_REQUIRE_DEPTH_BLIT_STATS:-0}"
REQUIRE_NON_BLACK_CAPTURE="${REALITYVK_PM_REQUIRE_NON_BLACK_CAPTURE:-1}"
VALIDATE_CACHED_REFERENCE_CAPTURE="${REALITYVK_PM_VALIDATE_CACHED_REFERENCE_CAPTURE:-0}"
CAPTURE_RETRY_COUNT="${REALITYVK_PM_CAPTURE_RETRY_COUNT:-6}"
CAPTURE_RETRY_STEP_FRAMES="${REALITYVK_PM_CAPTURE_RETRY_STEP_FRAMES:-20}"
CAPTURE_RETRY_RESUME_MS="${REALITYVK_PM_CAPTURE_RETRY_RESUME_MS:-250}"
CAPTURE_MIN_NONBLACK_RATIO="${REALITYVK_PM_CAPTURE_MIN_NONBLACK_RATIO:-0.001}"
CAPTURE_MIN_MEAN_LUMA="${REALITYVK_PM_CAPTURE_MIN_MEAN_LUMA:-0.002}"
# Paper Mario parity defaults to explicit agent-side flip so captures match live window orientation.
# Override with REALITYVK_PM_DUMPFB_FLIP_Y=0 when raw dump orientation is needed.
DUMPFB_FLIP_Y="${REALITYVK_PM_DUMPFB_FLIP_Y:-1}"
RVK2_PRESENT_FLIP_Y="${REALITYVK_PM_RVK2_PRESENT_FLIP_Y:-1}"
CAPTURE_SCALE_DIV="${REALITYVK_PM_CAPTURE_SCALE_DIV:-1}"
FRAMES_OVERRIDE="${REALITYVK_PM_FRAMES_OVERRIDE:-}"
LAUNCH_WITH_PTY="${REALITYVK_PM_LAUNCH_WITH_PTY:-1}"
DEEP_TELEMETRY="${REALITYVK_PM_DEEP_TELEMETRY:-0}"
TELEMETRY_ROOT="${REALITYVK_PM_TELEMETRY_ROOT:-${RUN_ROOT}/telemetry}"
DEEP_TELEMETRY_REPLAY_STRICT="${REALITYVK_PM_DEEP_TELEMETRY_REPLAY_STRICT:-0}"
DEEP_TELEMETRY_REPLAY_JOBS="${REALITYVK_PM_DEEP_TELEMETRY_REPLAY_JOBS:-0}"
DEEP_TELEMETRY_REPLAY_STATEFUL="${REALITYVK_PM_DEEP_TELEMETRY_REPLAY_STATEFUL:-0}"
DEEP_TELEMETRY_TRACE_LOG_SUMMARY="${REALITYVK_PM_DEEP_TELEMETRY_TRACE_LOG_SUMMARY:-1}"
DEEP_TELEMETRY_FBO_TRACE_LIMIT="${REALITYVK_PM_DEEP_TELEMETRY_FBO_TRACE_LIMIT:-20000}"
DEEP_TELEMETRY_READBACK_LIMIT="${REALITYVK_PM_DEEP_TELEMETRY_READBACK_LIMIT:-20000}"
DEEP_TELEMETRY_DIFF_PLAYBOOK="${REALITYVK_PM_DEEP_TELEMETRY_DIFF_PLAYBOOK:-1}"
DEEP_TELEMETRY_DIFF_THRESHOLD="${REALITYVK_PM_DEEP_TELEMETRY_DIFF_THRESHOLD:-20}"
DEEP_TELEMETRY_DIFF_MIN_AREA="${REALITYVK_PM_DEEP_TELEMETRY_DIFF_MIN_AREA:-256}"
DEEP_TELEMETRY_DIFF_MAX_BOXES="${REALITYVK_PM_DEEP_TELEMETRY_DIFF_MAX_BOXES:-32}"
DEEP_TELEMETRY_DIFF_DILATE="${REALITYVK_PM_DEEP_TELEMETRY_DIFF_DILATE:-1}"
DEEP_TELEMETRY_DIFF_MODE="${REALITYVK_PM_DEEP_TELEMETRY_DIFF_MODE:-missing_non_black}"
DEEP_TELEMETRY_DIFF_REF_NONBLACK_THRESHOLD="${REALITYVK_PM_DEEP_TELEMETRY_DIFF_REF_NONBLACK_THRESHOLD:-8}"
DEEP_TELEMETRY_DIFF_TEST_NONBLACK_THRESHOLD="${REALITYVK_PM_DEEP_TELEMETRY_DIFF_TEST_NONBLACK_THRESHOLD:-8}"
DEEP_TELEMETRY_DIFF_IGNORE_BOXES="${REALITYVK_PM_DEEP_TELEMETRY_DIFF_IGNORE_BOXES:-238,245,482,380}"
DEEP_TELEMETRY_COMMAND_CENSUS="${REALITYVK_PM_DEEP_TELEMETRY_COMMAND_CENSUS:-1}"
DEEP_TELEMETRY_COMMAND_FOCUS_WINDOW="${REALITYVK_PM_DEEP_TELEMETRY_COMMAND_FOCUS_WINDOW:-1}"
DEEP_TELEMETRY_HISTORY_MERGE_LOG="${REALITYVK_PM_DEEP_TELEMETRY_HISTORY_MERGE_LOG:-1}"
DEEP_TELEMETRY_OVERWRITE_LOG="${REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG:-1}"
DEEP_TELEMETRY_OVERWRITE_LOG_LIMIT="${REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_LIMIT:-200000}"
DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_BLACK_WRITES="${REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_BLACK_WRITES:-1}"
DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_ALL_WRITES="${REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_ALL_WRITES:-0}"
DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_TEXEL_DETAIL="${REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_TEXEL_DETAIL:-0}"
DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_IDS="${REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_IDS:-}"
DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_FROM_LAST_FOCUS="${REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_FROM_LAST_FOCUS:-1}"
DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_MAX="${REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_MAX:-64}"
DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_MIN="${REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_MIN:-}"
DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_MAX="${REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_MAX:-}"
DEEP_TELEMETRY_OVERWRITE_LOG_WORK_MIN="${REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_WORK_MIN:-}"
DEEP_TELEMETRY_OVERWRITE_LOG_WORK_MAX="${REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_WORK_MAX:-}"
DEEP_TELEMETRY_TRIANGLE_PACKET_LOG="${REALITYVK_PM_DEEP_TELEMETRY_TRIANGLE_PACKET_LOG:-1}"
DEEP_TELEMETRY_TRIANGLE_PACKET_LOG_LIMIT="${REALITYVK_PM_DEEP_TELEMETRY_TRIANGLE_PACKET_LOG_LIMIT:-200000}"
DEEP_TELEMETRY_EXECUTOR_PRESENT_DUMP="${REALITYVK_PM_DEEP_TELEMETRY_EXECUTOR_PRESENT_DUMP:-1}"
DEEP_TELEMETRY_EXECUTOR_PRESENT_DUMP_FRAME="${REALITYVK_PM_DEEP_TELEMETRY_EXECUTOR_PRESENT_DUMP_FRAME:-0}"
DEEP_TELEMETRY_REQUIRE_LIVE_PRESENT_SURFACE="${REALITYVK_PM_DEEP_TELEMETRY_REQUIRE_LIVE_PRESENT_SURFACE:-1}"
DEEP_TELEMETRY_MISSING_REGION_MAX_HIT_SAMPLES="${REALITYVK_PM_DEEP_TELEMETRY_MISSING_REGION_MAX_HIT_SAMPLES:-4096}"
DEEP_TELEMETRY_MISSING_REGION_HISTORY_WINDOW="${REALITYVK_PM_DEEP_TELEMETRY_MISSING_REGION_HISTORY_WINDOW:-4}"
DEEP_TELEMETRY_MISSING_REGION_MAX_ADDRESS_OVERLAP="${REALITYVK_PM_DEEP_TELEMETRY_MISSING_REGION_MAX_ADDRESS_OVERLAP:-16}"
DEEP_TELEMETRY_ARCHIVE="${REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE:-1}"
DEEP_TELEMETRY_ARCHIVE_ROOT="${REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE_ROOT:-${RUN_ROOT}/archive}"
DEEP_TELEMETRY_ARCHIVE_INDEX="${REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE_INDEX:-${DEEP_TELEMETRY_ARCHIVE_ROOT}/index.tsv}"
CANDIDATE_PLUGIN_FRESHNESS_CHECK="${REALITYVK_PM_CANDIDATE_PLUGIN_FRESHNESS_CHECK:-1}"
TELEMETRY_PRUNE_ENABLE="${REALITYVK_PM_TELEMETRY_PRUNE_ENABLE:-1}"
TELEMETRY_PRUNE_DRY_RUN="${REALITYVK_PM_TELEMETRY_PRUNE_DRY_RUN:-0}"
RVK2_ENABLE_SURFACE_HISTORY_BOOTSTRAP="${REALITYVK_PM_RVK2_ENABLE_SURFACE_HISTORY_BOOTSTRAP:-1}"
RVK2_ENABLE_CROSS_SURFACE_BOOTSTRAP="${REALITYVK_PM_RVK2_ENABLE_CROSS_SURFACE_BOOTSTRAP:-1}"
RVK2_DISABLE_VI_HISTORY_PRESENT="${REALITYVK_PM_RVK2_DISABLE_VI_HISTORY_PRESENT:-0}"
RVK2_PREFER_LIVE_SURFACE_OVER_HISTORY="${REALITYVK_PM_RVK2_PREFER_LIVE_SURFACE_OVER_HISTORY:-1}"
AUTO_COMPARE_VIEW="${REALITYVK_PM_AUTO_COMPARE_VIEW:-1}"
AUTO_COMPARE_CLOSE_ALL_EOG="${REALITYVK_PM_AUTO_COMPARE_CLOSE_ALL_EOG:-1}"

# GLideN64 cannot produce usable agentctl dumpfb-preset captures in our agent-mode flow.
# Keep this explicit so reference capture behavior is not misdiagnosed as an RVK2 regression.
REFERENCE_DUMPFB_INCOMPATIBLE=0
if [[ "${REFERENCE_PLUGIN,,}" == *"gliden64"* ]]; then
  REFERENCE_DUMPFB_INCOMPATIBLE=1
fi
REFERENCE_DUMPFB_WARNING_EMITTED=0

profile_default() {
  local value_name="$1"
  local env_name="$2"
  local default_value="$3"
  if [[ -z "${!env_name+x}" ]]; then
    printf -v "${value_name}" "%s" "${default_value}"
  fi
}

apply_profile_defaults() {
  case "${PROFILE}" in
    basic)
      profile_default "DEEP_TELEMETRY" "REALITYVK_PM_DEEP_TELEMETRY" "0"
      profile_default "DEEP_TELEMETRY_ARCHIVE" "REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE" "0"
      profile_default "DEEP_TELEMETRY_REPLAY_STATEFUL" "REALITYVK_PM_DEEP_TELEMETRY_REPLAY_STATEFUL" "0"
      ;;
    deep)
      profile_default "DEEP_TELEMETRY" "REALITYVK_PM_DEEP_TELEMETRY" "1"
      profile_default "DEEP_TELEMETRY_ARCHIVE" "REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE" "1"
      profile_default "DEEP_TELEMETRY_REPLAY_STATEFUL" "REALITYVK_PM_DEEP_TELEMETRY_REPLAY_STATEFUL" "1"
      profile_default "DEEP_TELEMETRY_DIFF_PLAYBOOK" "REALITYVK_PM_DEEP_TELEMETRY_DIFF_PLAYBOOK" "1"
      profile_default "DEEP_TELEMETRY_COMMAND_CENSUS" "REALITYVK_PM_DEEP_TELEMETRY_COMMAND_CENSUS" "1"
      profile_default "DEEP_TELEMETRY_HISTORY_MERGE_LOG" "REALITYVK_PM_DEEP_TELEMETRY_HISTORY_MERGE_LOG" "1"
      profile_default "DEEP_TELEMETRY_OVERWRITE_LOG" "REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG" "1"
      profile_default "DEEP_TELEMETRY_TRIANGLE_PACKET_LOG" "REALITYVK_PM_DEEP_TELEMETRY_TRIANGLE_PACKET_LOG" "1"
      profile_default "DEEP_TELEMETRY_EXECUTOR_PRESENT_DUMP" "REALITYVK_PM_DEEP_TELEMETRY_EXECUTOR_PRESENT_DUMP" "1"
      profile_default "DEEP_TELEMETRY_REQUIRE_LIVE_PRESENT_SURFACE" "REALITYVK_PM_DEEP_TELEMETRY_REQUIRE_LIVE_PRESENT_SURFACE" "1"
      profile_default "REQUIRE_NON_BLACK_CAPTURE" "REALITYVK_PM_REQUIRE_NON_BLACK_CAPTURE" "1"
      ;;
    *)
      rvk2_die_usage "REALITYVK_PM_PROFILE must be 'basic' or 'deep'."
      ;;
  esac
}

apply_profile_defaults

if [[ ! -f "${MANIFEST}" ]]; then
  echo "ERROR: scenario manifest not found: ${MANIFEST}" >&2
  exit 2
fi

if [[ "${REFRESH_REFERENCE}" != "0" && "${REFRESH_REFERENCE}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_REFRESH_REFERENCE must be 0 or 1." >&2
  exit 2
fi

if [[ "${VISUAL_GATE}" != "0" && "${VISUAL_GATE}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_VISUAL_GATE must be 0 or 1." >&2
  exit 2
fi

rvk2_require_enum "REALITYVK_PM_PROFILE" "${PROFILE}" "basic" "deep"
rvk2_require_bool "REALITYVK_PM_KNOB_TRACK_ENABLE" "${KNOB_TRACK_ENABLE}"
rvk2_require_uint_ge "REALITYVK_PM_KNOB_TRACK_WINDOW" "${KNOB_TRACK_WINDOW}" 1
rvk2_require_uint_ge "REALITYVK_PM_KNOB_TRACK_SUMMARY_LIMIT" "${KNOB_TRACK_SUMMARY_LIMIT}" 1
rvk2_require_bool "REALITYVK_PM_DRY_RUN" "${DRY_RUN}"

if [[ "${CAPTURE_DEPTH_SUMMARY}" != "0" && "${CAPTURE_DEPTH_SUMMARY}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_CAPTURE_DEPTH_SUMMARY must be 0 or 1." >&2
  exit 2
fi

if [[ "${REQUIRE_NO_DEPTH_BLIT_FAIL}" != "0" && "${REQUIRE_NO_DEPTH_BLIT_FAIL}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_REQUIRE_NO_DEPTH_BLIT_FAIL must be 0 or 1." >&2
  exit 2
fi

if [[ "${REQUIRE_DEPTH_BLIT_STATS}" != "0" && "${REQUIRE_DEPTH_BLIT_STATS}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_REQUIRE_DEPTH_BLIT_STATS must be 0 or 1." >&2
  exit 2
fi

if [[ "${REQUIRE_NON_BLACK_CAPTURE}" != "0" && "${REQUIRE_NON_BLACK_CAPTURE}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_REQUIRE_NON_BLACK_CAPTURE must be 0 or 1." >&2
  exit 2
fi

if [[ "${VALIDATE_CACHED_REFERENCE_CAPTURE}" != "0" && "${VALIDATE_CACHED_REFERENCE_CAPTURE}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_VALIDATE_CACHED_REFERENCE_CAPTURE must be 0 or 1." >&2
  exit 2
fi

if ! [[ "${CAPTURE_RETRY_COUNT}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_CAPTURE_RETRY_COUNT must be an integer >= 0." >&2
  exit 2
fi

rvk2_require_uint_ge "REALITYVK_PM_CAPTURE_RETRY_STEP_FRAMES" "${CAPTURE_RETRY_STEP_FRAMES}" 0
rvk2_require_uint_ge "REALITYVK_PM_CAPTURE_RETRY_RESUME_MS" "${CAPTURE_RETRY_RESUME_MS}" 0
rvk2_require_float "REALITYVK_PM_CAPTURE_MIN_NONBLACK_RATIO" "${CAPTURE_MIN_NONBLACK_RATIO}"
rvk2_require_float "REALITYVK_PM_CAPTURE_MIN_MEAN_LUMA" "${CAPTURE_MIN_MEAN_LUMA}"
rvk2_require_bool "REALITYVK_PM_DUMPFB_FLIP_Y" "${DUMPFB_FLIP_Y}"
rvk2_require_bool "REALITYVK_PM_RVK2_PRESENT_FLIP_Y" "${RVK2_PRESENT_FLIP_Y}"
if [[ -n "${FRAMES_OVERRIDE}" ]]; then
  rvk2_require_uint_ge "REALITYVK_PM_FRAMES_OVERRIDE" "${FRAMES_OVERRIDE}" 0
fi
rvk2_require_bool "REALITYVK_PM_LAUNCH_WITH_PTY" "${LAUNCH_WITH_PTY}"
rvk2_require_bool "REALITYVK_PM_DEEP_TELEMETRY" "${DEEP_TELEMETRY}"
rvk2_require_bool "REALITYVK_PM_DEEP_TELEMETRY_REPLAY_STRICT" "${DEEP_TELEMETRY_REPLAY_STRICT}"
rvk2_require_bool "REALITYVK_PM_DEEP_TELEMETRY_REPLAY_STATEFUL" "${DEEP_TELEMETRY_REPLAY_STATEFUL}"
rvk2_require_bool "REALITYVK_PM_DEEP_TELEMETRY_TRACE_LOG_SUMMARY" "${DEEP_TELEMETRY_TRACE_LOG_SUMMARY}"
rvk2_require_uint_ge "REALITYVK_PM_DEEP_TELEMETRY_REPLAY_JOBS" "${DEEP_TELEMETRY_REPLAY_JOBS}" 0

if ! [[ "${DEEP_TELEMETRY_FBO_TRACE_LIMIT}" =~ ^[0-9]+$ ]] || [[ "${DEEP_TELEMETRY_FBO_TRACE_LIMIT}" == "0" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_FBO_TRACE_LIMIT must be an integer >= 1." >&2
  exit 2
fi

if ! [[ "${DEEP_TELEMETRY_READBACK_LIMIT}" =~ ^[0-9]+$ ]] || [[ "${DEEP_TELEMETRY_READBACK_LIMIT}" == "0" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_READBACK_LIMIT must be an integer >= 1." >&2
  exit 2
fi

if [[ "${DEEP_TELEMETRY_DIFF_PLAYBOOK}" != "0" && "${DEEP_TELEMETRY_DIFF_PLAYBOOK}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_DIFF_PLAYBOOK must be 0 or 1." >&2
  exit 2
fi

if [[ "${DEEP_TELEMETRY_COMMAND_CENSUS}" != "0" && "${DEEP_TELEMETRY_COMMAND_CENSUS}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_COMMAND_CENSUS must be 0 or 1." >&2
  exit 2
fi

if [[ "${DEEP_TELEMETRY_HISTORY_MERGE_LOG}" != "0" && "${DEEP_TELEMETRY_HISTORY_MERGE_LOG}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_HISTORY_MERGE_LOG must be 0 or 1." >&2
  exit 2
fi

if [[ "${DEEP_TELEMETRY_OVERWRITE_LOG}" != "0" && "${DEEP_TELEMETRY_OVERWRITE_LOG}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG must be 0 or 1." >&2
  exit 2
fi

if [[ "${DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_BLACK_WRITES}" != "0" && "${DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_BLACK_WRITES}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_BLACK_WRITES must be 0 or 1." >&2
  exit 2
fi

if [[ "${DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_ALL_WRITES}" != "0" && "${DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_ALL_WRITES}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_ALL_WRITES must be 0 or 1." >&2
  exit 2
fi

if [[ "${DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_TEXEL_DETAIL}" != "0" && "${DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_TEXEL_DETAIL}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_TEXEL_DETAIL must be 0 or 1." >&2
  exit 2
fi

if [[ "${DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_FROM_LAST_FOCUS}" != "0" && "${DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_FROM_LAST_FOCUS}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_FROM_LAST_FOCUS must be 0 or 1." >&2
  exit 2
fi

if [[ "${DEEP_TELEMETRY_TRIANGLE_PACKET_LOG}" != "0" && "${DEEP_TELEMETRY_TRIANGLE_PACKET_LOG}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_TRIANGLE_PACKET_LOG must be 0 or 1." >&2
  exit 2
fi

if [[ "${DEEP_TELEMETRY_EXECUTOR_PRESENT_DUMP}" != "0" && "${DEEP_TELEMETRY_EXECUTOR_PRESENT_DUMP}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_EXECUTOR_PRESENT_DUMP must be 0 or 1." >&2
  exit 2
fi

if [[ "${DEEP_TELEMETRY_REQUIRE_LIVE_PRESENT_SURFACE}" != "0" && "${DEEP_TELEMETRY_REQUIRE_LIVE_PRESENT_SURFACE}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_REQUIRE_LIVE_PRESENT_SURFACE must be 0 or 1." >&2
  exit 2
fi

if [[ "${RVK2_ENABLE_SURFACE_HISTORY_BOOTSTRAP}" != "0" && "${RVK2_ENABLE_SURFACE_HISTORY_BOOTSTRAP}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_RVK2_ENABLE_SURFACE_HISTORY_BOOTSTRAP must be 0 or 1." >&2
  exit 2
fi

if [[ "${RVK2_ENABLE_CROSS_SURFACE_BOOTSTRAP}" != "0" && "${RVK2_ENABLE_CROSS_SURFACE_BOOTSTRAP}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_RVK2_ENABLE_CROSS_SURFACE_BOOTSTRAP must be 0 or 1." >&2
  exit 2
fi

rvk2_require_bool "REALITYVK_PM_RVK2_DISABLE_VI_HISTORY_PRESENT" "${RVK2_DISABLE_VI_HISTORY_PRESENT}"
rvk2_require_bool "REALITYVK_PM_RVK2_PREFER_LIVE_SURFACE_OVER_HISTORY" "${RVK2_PREFER_LIVE_SURFACE_OVER_HISTORY}"
rvk2_require_bool "REALITYVK_PM_AUTO_COMPARE_VIEW" "${AUTO_COMPARE_VIEW}"
rvk2_require_bool "REALITYVK_PM_AUTO_COMPARE_CLOSE_ALL_EOG" "${AUTO_COMPARE_CLOSE_ALL_EOG}"

if ! [[ "${DEEP_TELEMETRY_DIFF_THRESHOLD}" =~ ^[0-9]+$ ]] || (( DEEP_TELEMETRY_DIFF_THRESHOLD > 255 )); then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_DIFF_THRESHOLD must be an integer in [0,255]." >&2
  exit 2
fi

if ! [[ "${DEEP_TELEMETRY_DIFF_MIN_AREA}" =~ ^[0-9]+$ ]] || [[ "${DEEP_TELEMETRY_DIFF_MIN_AREA}" == "0" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_DIFF_MIN_AREA must be an integer >= 1." >&2
  exit 2
fi

if ! [[ "${DEEP_TELEMETRY_DIFF_MAX_BOXES}" =~ ^[0-9]+$ ]] || [[ "${DEEP_TELEMETRY_DIFF_MAX_BOXES}" == "0" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_DIFF_MAX_BOXES must be an integer >= 1." >&2
  exit 2
fi

if ! [[ "${DEEP_TELEMETRY_DIFF_DILATE}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_DIFF_DILATE must be an integer >= 0." >&2
  exit 2
fi

if [[ "${DEEP_TELEMETRY_DIFF_MODE}" != "absdiff" && "${DEEP_TELEMETRY_DIFF_MODE}" != "missing_non_black" && "${DEEP_TELEMETRY_DIFF_MODE}" != "extra_non_black" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_DIFF_MODE must be 'absdiff', 'missing_non_black', or 'extra_non_black'." >&2
  exit 2
fi

if ! [[ "${DEEP_TELEMETRY_DIFF_REF_NONBLACK_THRESHOLD}" =~ ^[0-9]+$ ]] || (( DEEP_TELEMETRY_DIFF_REF_NONBLACK_THRESHOLD > 255 )); then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_DIFF_REF_NONBLACK_THRESHOLD must be an integer in [0,255]." >&2
  exit 2
fi

if ! [[ "${DEEP_TELEMETRY_DIFF_TEST_NONBLACK_THRESHOLD}" =~ ^[0-9]+$ ]] || (( DEEP_TELEMETRY_DIFF_TEST_NONBLACK_THRESHOLD > 255 )); then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_DIFF_TEST_NONBLACK_THRESHOLD must be an integer in [0,255]." >&2
  exit 2
fi

if ! [[ "${DEEP_TELEMETRY_COMMAND_FOCUS_WINDOW}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_COMMAND_FOCUS_WINDOW must be an integer >= 0." >&2
  exit 2
fi

if ! [[ "${DEEP_TELEMETRY_MISSING_REGION_MAX_HIT_SAMPLES}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_MISSING_REGION_MAX_HIT_SAMPLES must be an integer >= 0." >&2
  exit 2
fi

if ! [[ "${DEEP_TELEMETRY_MISSING_REGION_HISTORY_WINDOW}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_MISSING_REGION_HISTORY_WINDOW must be an integer >= 0." >&2
  exit 2
fi

if ! [[ "${DEEP_TELEMETRY_MISSING_REGION_MAX_ADDRESS_OVERLAP}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_MISSING_REGION_MAX_ADDRESS_OVERLAP must be an integer >= 0." >&2
  exit 2
fi

if ! [[ "${DEEP_TELEMETRY_OVERWRITE_LOG_LIMIT}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_LIMIT must be an integer >= 0." >&2
  exit 2
fi

if ! [[ "${DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_MAX}" =~ ^[0-9]+$ ]] || [[ "${DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_MAX}" == "0" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_MAX must be an integer >= 1." >&2
  exit 2
fi

if ! [[ "${DEEP_TELEMETRY_TRIANGLE_PACKET_LOG_LIMIT}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_TRIANGLE_PACKET_LOG_LIMIT must be an integer >= 0." >&2
  exit 2
fi

if [[ -n "${DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_MIN}" ]] && ! [[ "${DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_MIN}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_MIN must be an integer >= 0 when set." >&2
  exit 2
fi

if [[ -n "${DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_MAX}" ]] && ! [[ "${DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_MAX}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_MAX must be an integer >= 0 when set." >&2
  exit 2
fi

if [[ -n "${DEEP_TELEMETRY_OVERWRITE_LOG_WORK_MIN}" ]] && ! [[ "${DEEP_TELEMETRY_OVERWRITE_LOG_WORK_MIN}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_WORK_MIN must be an integer >= 0 when set." >&2
  exit 2
fi

if [[ -n "${DEEP_TELEMETRY_OVERWRITE_LOG_WORK_MAX}" ]] && ! [[ "${DEEP_TELEMETRY_OVERWRITE_LOG_WORK_MAX}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_WORK_MAX must be an integer >= 0 when set." >&2
  exit 2
fi

if ! [[ "${DEEP_TELEMETRY_EXECUTOR_PRESENT_DUMP_FRAME}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_EXECUTOR_PRESENT_DUMP_FRAME must be an integer >= 0." >&2
  exit 2
fi

if [[ "${DEEP_TELEMETRY_ARCHIVE}" != "0" && "${DEEP_TELEMETRY_ARCHIVE}" != "1" ]]; then
  echo "ERROR: REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE must be 0 or 1." >&2
  exit 2
fi

rvk2_require_bool "REALITYVK_PM_CANDIDATE_PLUGIN_FRESHNESS_CHECK" "${CANDIDATE_PLUGIN_FRESHNESS_CHECK}"
rvk2_require_bool "REALITYVK_PM_TELEMETRY_PRUNE_ENABLE" "${TELEMETRY_PRUNE_ENABLE}"
rvk2_require_bool "REALITYVK_PM_TELEMETRY_PRUNE_DRY_RUN" "${TELEMETRY_PRUNE_DRY_RUN}"

if [[ ! -f "${CANDIDATE_PLUGIN}" ]]; then
  echo "ERROR: candidate plugin not found: ${CANDIDATE_PLUGIN}" >&2
  exit 2
fi

if [[ ! -f "${REFERENCE_CORELIB}" ]]; then
  echo "ERROR: reference core library not found: ${REFERENCE_CORELIB}" >&2
  exit 2
fi

if [[ ! -f "${CANDIDATE_CORELIB}" ]]; then
  echo "ERROR: candidate core library not found: ${CANDIDATE_CORELIB}" >&2
  exit 2
fi

require_candidate_plugin_canonical_telemetry_keys() {
  if [[ "${DEEP_TELEMETRY}" != "1" || "${CANDIDATE_PLUGIN_FRESHNESS_CHECK}" != "1" || "${DRY_RUN}" == "1" ]]; then
    return 0
  fi
  if ! command -v strings >/dev/null 2>&1; then
    echo "WARN: skipping candidate plugin freshness check; 'strings' is unavailable." >&2
    return 0
  fi

  local plugin_strings=""
  plugin_strings="$(strings "${CANDIDATE_PLUGIN}" 2>/dev/null || true)"
  if [[ -z "${plugin_strings}" ]]; then
    echo "WARN: skipping candidate plugin freshness check; no symbols were extracted from ${CANDIDATE_PLUGIN}." >&2
    return 0
  fi

  local -a required_keys=(
    "REALITYVK_RVK2_TRACE_FILE"
    "REALITYVK_RVK2_PACKET_TRACE_FILE"
    "REALITYVK_RVK2_FRAME_FORENSICS_FILE"
  )
  local -a missing_keys=()
  local key=""
  for key in "${required_keys[@]}"; do
    if ! grep -Fq "${key}" <<< "${plugin_strings}"; then
      missing_keys+=("${key}")
    fi
  done

  if ((${#missing_keys[@]} == 0)); then
    return 0
  fi

  echo "ERROR: candidate plugin appears stale for deep telemetry; missing canonical env keys:" >&2
  for key in "${missing_keys[@]}"; do
    echo "  - ${key}" >&2
  done
  if grep -Fq "REALITYVK2_PACKET_TRACE_FILE" <<< "${plugin_strings}"; then
    echo "ERROR: legacy REALITYVK2_* telemetry keys detected; rebuild is required." >&2
  fi
  echo "ERROR: rebuild candidate plugin before deep smoke:" >&2
  echo "  cmake -S src -B build/release-vulkan-smoke -DCMAKE_BUILD_TYPE=Release" >&2
  echo "  cmake --build build/release-vulkan-smoke -j\$(nproc)" >&2
  exit 2
}

prune_telemetry_root_if_enabled() {
  if [[ "${DEEP_TELEMETRY}" != "1" || "${TELEMETRY_PRUNE_ENABLE}" != "1" || "${DRY_RUN}" == "1" ]]; then
    return 0
  fi
  local cleanup_script="${ROOT_DIR}/scripts/paper_mario_telemetry_cleanup.sh"
  if [[ ! -f "${cleanup_script}" ]]; then
    echo "WARN: telemetry cleanup script not found; skipping prune: ${cleanup_script}" >&2
    return 0
  fi
  "${cleanup_script}" \
    --run-root "${RUN_ROOT}" \
    --scenario-id "${SCENARIO_ID}" \
    --dry-run "${TELEMETRY_PRUNE_DRY_RUN}"
}

require_candidate_plugin_canonical_telemetry_keys

scenario_line="$(
  awk -F '\t' -v id="${SCENARIO_ID}" '
    $0 ~ /^[[:space:]]*#/ { next }
    $1 == id { print; exit }
  ' "${MANIFEST}"
)"

if [[ -z "${scenario_line}" ]]; then
  echo "ERROR: scenario '${SCENARIO_ID}' not found in ${MANIFEST}" >&2
  exit 2
fi

IFS=$'\t' read -r _scenario ROM_PATH FRAMES SCENARIO_ARGS <<< "${scenario_line}"
FRAMES="${FRAMES:-0}"
SCENARIO_FRAMES="${FRAMES}"
SCENARIO_ARGS="${SCENARIO_ARGS:-}"

if [[ -z "${ROM_PATH}" || ! -f "${ROM_PATH}" ]]; then
  echo "ERROR: ROM path for scenario '${SCENARIO_ID}' is missing or does not exist: ${ROM_PATH}" >&2
  exit 2
fi

if ! [[ "${FRAMES}" =~ ^[0-9]+$ ]]; then
  echo "ERROR: frames value for scenario '${SCENARIO_ID}' is invalid: ${FRAMES}" >&2
  exit 2
fi

if [[ -n "${FRAMES_OVERRIDE}" ]]; then
  FRAMES="${FRAMES_OVERRIDE}"
  echo "INFO: frames override active: scenario_frames=${SCENARIO_FRAMES} effective_frames=${FRAMES}"
fi

mkdir -p "${CACHE_ROOT}" "${RUN_ROOT}"
prune_telemetry_root_if_enabled

if [[ "${DEEP_TELEMETRY}" == "1" && "${SCENARIO_ID}" != "paper_mario_intro" ]]; then
  echo "WARN: deep telemetry profile is tuned for paper_mario_intro (running ${SCENARIO_ID})." >&2
fi

REFERENCE_CAPTURE=""
CANDIDATE_CAPTURE="${RUN_ROOT}/${SCENARIO_ID}.candidate.ppm"
DIFF_OUT="${RUN_ROOT}/${SCENARIO_ID}.diff.png"
METRICS_OUT="${RUN_ROOT}/${SCENARIO_ID}.metrics.json"
CAPTURE_CONTEXT_OUT="${RUN_ROOT}/${SCENARIO_ID}.capture-context.json"
REFERENCE_PNG="${RUN_ROOT}/${SCENARIO_ID}.reference.png"
CANDIDATE_PNG="${RUN_ROOT}/${SCENARIO_ID}.candidate.png"

VISUAL_COMPARE_EXIT_CODE="0"
DEEP_REPLAY_EXIT_CODE="-1"
DEEP_FORENSICS_SUMMARY_EXIT_CODE="-1"
DEEP_FORENSICS_ACTIVE_SUMMARY_EXIT_CODE="-1"
TELEMETRY_BUNDLE_OUT=""
CANDIDATE_TRACE_OUT=""
CANDIDATE_PACKET_TRACE_OUT=""
CANDIDATE_PACKET_REPLAY_OUT=""
CANDIDATE_FRAME_FORENSICS_OUT=""
CANDIDATE_FRAME_FORENSICS_SUMMARY_OUT=""
CANDIDATE_FRAME_FORENSICS_ACTIVE_SUMMARY_OUT=""
CANDIDATE_LAUNCH_LOG_OUT=""
CANDIDATE_DEPTH_SUMMARY_OUT=""
CANDIDATE_MISSING_REGION_FOCUS_OUT=""
DEVIATION_OUT_DIR=""
DIFF_PLAYBOOK_SUMMARY_OUT=""
DIFF_PLAYBOOK_BOXES_OUT=""
DIFF_PLAYBOOK_SNIPPET_OUT=""
COMMAND_CENSUS_OUT=""
COMMAND_CENSUS_MD_OUT=""
CANDIDATE_HISTORY_MERGE_LOG_OUT=""
CANDIDATE_OVERWRITE_LOG_OUT=""
CANDIDATE_TRIANGLE_PACKET_LOG_OUT=""
CANDIDATE_EXECUTOR_PRESENT_DUMP_OUT=""
DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_IDS_EFFECTIVE="${DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_IDS}"
COMPARE_SIDE_BY_SIDE_OUT="${RUN_ROOT}/${SCENARIO_ID}.compare_side_by_side.latest.png"
COMPARE_VIEWER_PID_FILE="${RUN_ROOT}/.paper_mario_parity_compare_view.pid"
DEEP_ARCHIVE_RUN_STAMP="$(date -u +%Y%m%d-%H%M%SZ)"
DEEP_ARCHIVE_GIT_SHA="$(git -C "${ROOT_DIR}" rev-parse --short HEAD 2>/dev/null || echo "nogit")"
DEEP_ARCHIVE_RUN_ID="${SCENARIO_ID}.${DEEP_ARCHIVE_RUN_STAMP}.${DEEP_ARCHIVE_GIT_SHA}"
DEEP_ARCHIVE_DIR="${DEEP_TELEMETRY_ARCHIVE_ROOT}/${DEEP_ARCHIVE_RUN_ID}"
KNOB_SNAPSHOT_OUT="${RUN_ROOT}/${SCENARIO_ID}.knobs.${DEEP_ARCHIVE_RUN_STAMP}.${DEEP_ARCHIVE_GIT_SHA}.json"
KNOB_SNAPSHOT_LATEST_OUT="${RUN_ROOT}/${SCENARIO_ID}.knobs.latest.json"
KNOB_FINGERPRINT=""
DRY_RUN_CAPTURE_RECORDS_FILE="${RUN_ROOT}/${SCENARIO_ID}.dry-run.captures.${DEEP_ARCHIVE_RUN_STAMP}.jsonl"

SCENARIO_ARGS_ARRAY=()
if [[ -n "${SCENARIO_ARGS// }" ]]; then
  # shellcheck disable=SC2206
  SCENARIO_ARGS_ARRAY=(${SCENARIO_ARGS})
fi

if ! [[ "${CAPTURE_SCALE_DIV}" =~ ^[0-9]+$ ]] || [[ "${CAPTURE_SCALE_DIV}" == "0" ]]; then
  echo "ERROR: REALITYVK_PM_CAPTURE_SCALE_DIV must be an integer >= 1." >&2
  exit 2
fi

if ((${#SCENARIO_ARGS_ARRAY[@]})); then
  for ((arg_i = 0; arg_i < ${#SCENARIO_ARGS_ARRAY[@]}; ++arg_i)); do
    token="${SCENARIO_ARGS_ARRAY[arg_i]}"
    if [[ "${token}" == "--scale-div" ]]; then
      if ((arg_i + 1 < ${#SCENARIO_ARGS_ARRAY[@]})); then
        CAPTURE_SCALE_DIV="${SCENARIO_ARGS_ARRAY[arg_i + 1]}"
      fi
      continue
    fi
    if [[ "${token}" == --scale-div=* ]]; then
      CAPTURE_SCALE_DIV="${token#--scale-div=}"
      continue
    fi
  done
fi

if ! [[ "${CAPTURE_SCALE_DIV}" =~ ^[0-9]+$ ]] || [[ "${CAPTURE_SCALE_DIV}" == "0" ]]; then
  echo "ERROR: capture scale-div must resolve to an integer >= 1." >&2
  exit 2
fi

if [[ "${DEEP_TELEMETRY}" == "1" ]]; then
  mkdir -p "${TELEMETRY_ROOT}"
  CANDIDATE_TRACE_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.trace.tsv"
  CANDIDATE_PACKET_TRACE_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.packet.tsv"
  CANDIDATE_PACKET_REPLAY_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.packet.replay.json"
  CANDIDATE_FRAME_FORENSICS_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.frame-forensics.tsv"
  CANDIDATE_FRAME_FORENSICS_SUMMARY_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.frame-forensics.summary.txt"
  CANDIDATE_FRAME_FORENSICS_ACTIVE_SUMMARY_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.frame-forensics.active.summary.txt"
  CANDIDATE_LAUNCH_LOG_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.launch.log"
  CANDIDATE_DEPTH_SUMMARY_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.depth-blit-summary.json"
  CANDIDATE_MISSING_REGION_FOCUS_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.missing-region-focus.json"
  DEVIATION_OUT_DIR="${TELEMETRY_ROOT}/${SCENARIO_ID}.deviation"
  DIFF_PLAYBOOK_SUMMARY_OUT="${DEVIATION_OUT_DIR}/summary.json"
  DIFF_PLAYBOOK_BOXES_OUT="${DEVIATION_OUT_DIR}/boxes.json"
  DIFF_PLAYBOOK_SNIPPET_OUT="${DEVIATION_OUT_DIR}/playbook_snippet.md"
  COMMAND_CENSUS_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.command-census.json"
  COMMAND_CENSUS_MD_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.command-census.md"
  CANDIDATE_HISTORY_MERGE_LOG_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.history-merge.tsv"
  CANDIDATE_OVERWRITE_LOG_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.overwrite.tsv"
  CANDIDATE_TRIANGLE_PACKET_LOG_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.triangle-packet.tsv"
  CANDIDATE_EXECUTOR_PRESENT_DUMP_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.candidate.executor-present.ppm"
  TELEMETRY_BUNDLE_OUT="${TELEMETRY_ROOT}/${SCENARIO_ID}.telemetry.bundle.json"
  rm -rf "${DEVIATION_OUT_DIR}"
  rm -f \
    "${CANDIDATE_TRACE_OUT}" \
    "${CANDIDATE_PACKET_TRACE_OUT}" \
    "${CANDIDATE_PACKET_REPLAY_OUT}" \
    "${CANDIDATE_FRAME_FORENSICS_OUT}" \
    "${CANDIDATE_FRAME_FORENSICS_SUMMARY_OUT}" \
    "${CANDIDATE_FRAME_FORENSICS_ACTIVE_SUMMARY_OUT}" \
    "${CANDIDATE_LAUNCH_LOG_OUT}" \
    "${CANDIDATE_DEPTH_SUMMARY_OUT}" \
    "${CANDIDATE_MISSING_REGION_FOCUS_OUT}" \
    "${COMMAND_CENSUS_OUT}" \
    "${COMMAND_CENSUS_MD_OUT}" \
    "${CANDIDATE_HISTORY_MERGE_LOG_OUT}" \
    "${CANDIDATE_OVERWRITE_LOG_OUT}" \
    "${CANDIDATE_TRIANGLE_PACKET_LOG_OUT}" \
    "${CANDIDATE_EXECUTOR_PRESENT_DUMP_OUT}" \
    "${TELEMETRY_BUNDLE_OUT}"
fi

CAPTURE_METHOD_TAG="dumpfb_scale${CAPTURE_SCALE_DIV}_flip${DUMPFB_FLIP_Y}"
REFERENCE_CAPTURE="${CACHE_ROOT}/${SCENARIO_ID}.${CAPTURE_METHOD_TAG}.reference.ppm"

capture_plugin() {
  local label="$1"
  local plugin_path="$2"
  local out_path="$3"
  local corelib_path="${CANDIDATE_CORELIB}"
  local require_no_depth_fail="0"
  local require_depth_stats="0"
  local require_non_black_capture="${REQUIRE_NON_BLACK_CAPTURE}"
  local rvk2_present_flip_y=""
  local depth_summary_out=""
  if [[ "${label}" == "reference" ]]; then
    corelib_path="${REFERENCE_CORELIB}"
    if [[ "${REFERENCE_DUMPFB_INCOMPATIBLE}" == "1" ]]; then
      require_non_black_capture="0"
    fi
  fi
  if [[ "${label}" == "candidate" ]]; then
    require_no_depth_fail="${REQUIRE_NO_DEPTH_BLIT_FAIL}"
    require_depth_stats="${REQUIRE_DEPTH_BLIT_STATS}"
    rvk2_present_flip_y="${RVK2_PRESENT_FLIP_Y}"
  fi
  if [[ "${CAPTURE_DEPTH_SUMMARY}" == "1" ]]; then
    if [[ "${label}" == "candidate" && "${DEEP_TELEMETRY}" == "1" ]]; then
      depth_summary_out="${CANDIDATE_DEPTH_SUMMARY_OUT}"
    else
      depth_summary_out="${RUN_ROOT}/${SCENARIO_ID}.${label}.depth-blit-summary.json"
    fi
  fi

  local -a cmd=(
    "${ROOT_DIR}/scripts/paper_mario_smoke_runner.sh"
    --backend "Vulkan"
    --rom "${ROM_PATH}"
    --frames "${FRAMES}"
    --out "${out_path}"
  )
  if ((${#SCENARIO_ARGS_ARRAY[@]})); then
    cmd+=("${SCENARIO_ARGS_ARRAY[@]}")
  fi

  local -a run_env=(
    "M64_CORELIB=${corelib_path}"
    "REALITYVK_SMOKE_PLUGIN_VULKAN=${plugin_path}"
    "REALITYVK_SMOKE_REQUIRE_NO_DEPTH_BLIT_FAIL=${require_no_depth_fail}"
    "REALITYVK_SMOKE_REQUIRE_DEPTH_BLIT_STATS=${require_depth_stats}"
    "REALITYVK_SMOKE_DEPTH_BLIT_SUMMARY_OUT=${depth_summary_out}"
    "REALITYVK_SMOKE_REQUIRE_NON_BLACK_CAPTURE=${require_non_black_capture}"
    "REALITYVK_SMOKE_CAPTURE_RETRY_COUNT=${CAPTURE_RETRY_COUNT}"
    "REALITYVK_SMOKE_CAPTURE_RETRY_STEP_FRAMES=${CAPTURE_RETRY_STEP_FRAMES}"
    "REALITYVK_SMOKE_CAPTURE_RETRY_RESUME_MS=${CAPTURE_RETRY_RESUME_MS}"
    "REALITYVK_SMOKE_CAPTURE_MIN_NONBLACK_RATIO=${CAPTURE_MIN_NONBLACK_RATIO}"
    "REALITYVK_SMOKE_CAPTURE_MIN_MEAN_LUMA=${CAPTURE_MIN_MEAN_LUMA}"
    "REALITYVK_SMOKE_DUMPFB_FLIP_Y=${DUMPFB_FLIP_Y}"
    "REALITYVK_RVK2_PRESENT_FLIP_Y=${rvk2_present_flip_y}"
    "REALITYVK_SMOKE_LAUNCH_WITH_PTY=${LAUNCH_WITH_PTY}"
  )
  if [[ "${label}" == "candidate" && "${DEEP_TELEMETRY}" == "1" ]]; then
    rvk2_pm_append_candidate_deep_telemetry_env run_env
  fi
  if [[ "${label}" == "candidate" ]]; then
    rvk2_pm_append_candidate_debug_env run_env
  fi
  if [[ "${DRY_RUN}" == "1" ]]; then
    python3 - \
      "${DRY_RUN_CAPTURE_RECORDS_FILE}" \
      "${label}" \
      "${out_path}" \
      "__RVK2_CMD_BEGIN__" \
      "${cmd[@]}" \
      "__RVK2_ENV_BEGIN__" \
      "${run_env[@]}" <<'PY'
import json
import sys
from pathlib import Path

records_path = Path(sys.argv[1])
label = sys.argv[2]
out_path = sys.argv[3]
tokens = sys.argv[4:]

cmd_marker = "__RVK2_CMD_BEGIN__"
env_marker = "__RVK2_ENV_BEGIN__"
if cmd_marker not in tokens or env_marker not in tokens:
    raise SystemExit("missing dry-run capture markers")

cmd_start = tokens.index(cmd_marker) + 1
env_start = tokens.index(env_marker)
cmd = tokens[cmd_start:env_start]
env = tokens[env_start + 1 :]

records_path.parent.mkdir(parents=True, exist_ok=True)
with records_path.open("a", encoding="utf-8") as handle:
    handle.write(
        json.dumps(
            {
                "label": label,
                "out_path": out_path,
                "command": cmd,
                "env": env,
            }
        )
        + "\n"
    )
PY
    return 0
  fi

  env "${run_env[@]}" "${cmd[@]}"
}

warn_reference_dumpfb_incompatibility() {
  if [[ "${REFERENCE_DUMPFB_INCOMPATIBLE}" != "1" ]]; then
    return 0
  fi
  if [[ "${REFERENCE_DUMPFB_WARNING_EMITTED}" == "1" ]]; then
    return 0
  fi
  REFERENCE_DUMPFB_WARNING_EMITTED=1
  cat >&2 <<EOF_WARN
WARN: reference plugin appears to be GLideN64: ${REFERENCE_PLUGIN}
WARN: GLideN64 does not produce usable agentctl dumpfb-preset output in this workflow.
WARN: reference non-black capture validation is disabled for this run.
EOF_WARN
}

emit_dry_run_plan_and_exit() {
  python3 - \
    "${DRY_RUN_CAPTURE_RECORDS_FILE}" \
    "${DRY_RUN_OUT}" \
    "${SCENARIO_ID}" \
    "${PROFILE}" \
    "${DEEP_TELEMETRY}" \
    "${CAPTURE_METHOD_TAG}" \
    "${REFERENCE_CAPTURE}" \
    "${CANDIDATE_CAPTURE}" \
    "${ROM_PATH}" \
    "${FRAMES}" \
    "${KNOB_FINGERPRINT}" \
    "${KNOB_SNAPSHOT_OUT}" \
    "${DEEP_ARCHIVE_GIT_SHA}" <<'PY'
import json
import sys
from pathlib import Path

records_path = Path(sys.argv[1])
out_path = Path(sys.argv[2])
scenario_id = sys.argv[3]
profile = sys.argv[4]
deep_telemetry = int(sys.argv[5])
capture_method_tag = sys.argv[6]
reference_capture = sys.argv[7]
candidate_capture = sys.argv[8]
rom_path = sys.argv[9]
frames = int(sys.argv[10])
knob_fingerprint = sys.argv[11]
knob_snapshot = sys.argv[12]
git_sha = sys.argv[13]

captures = []
if records_path.is_file():
    for line in records_path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            row = json.loads(line)
        except Exception:
            continue
        if isinstance(row, dict):
            captures.append(row)

payload = {
    "dry_run": True,
    "scenario_id": scenario_id,
    "profile": profile,
    "deep_telemetry": deep_telemetry,
    "capture_method_tag": capture_method_tag,
    "git_sha": git_sha,
    "rom_path": rom_path,
    "frames": frames,
    "reference_capture": reference_capture,
    "candidate_capture": candidate_capture,
    "knob_fingerprint": knob_fingerprint or None,
    "knob_snapshot": knob_snapshot,
    "captures": captures,
}

out_path.parent.mkdir(parents=True, exist_ok=True)
out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
print(out_path)
PY
  echo "dry run plan: ${DRY_RUN_OUT}"
  exit 0
}

capture_has_content() {
  local capture_path="$1"
  python3 - "${capture_path}" "${CAPTURE_MIN_NONBLACK_RATIO}" "${CAPTURE_MIN_MEAN_LUMA}" <<'PY'
import sys
from pathlib import Path

import numpy as np
from PIL import Image

capture_path = Path(sys.argv[1])
min_nonblack_ratio = float(sys.argv[2])
min_mean_luma = float(sys.argv[3])

if not capture_path.is_file():
    raise SystemExit(1)

arr = np.asarray(Image.open(capture_path).convert("RGB"), dtype=np.float32) / 255.0
if arr.size == 0:
    raise SystemExit(1)

non_black_ratio = float(np.any(arr > 0.0, axis=2).mean())
luma = 0.2126 * arr[..., 0] + 0.7152 * arr[..., 1] + 0.0722 * arr[..., 2]
mean_luma = float(luma.mean())

if non_black_ratio >= min_nonblack_ratio and mean_luma >= min_mean_luma:
    raise SystemExit(0)
raise SystemExit(1)
PY
}

copy_artifact_if_file() {
  local src="$1"
  local dst="$2"
  if [[ -f "${src}" ]]; then
    mkdir -p "$(dirname "${dst}")"
    cp -f "${src}" "${dst}"
  fi
}

archive_deep_telemetry_run() {
  if [[ "${DEEP_TELEMETRY}" != "1" || "${DEEP_TELEMETRY_ARCHIVE}" != "1" ]]; then
    return 0
  fi

  mkdir -p "${DEEP_ARCHIVE_DIR}" "${DEEP_ARCHIVE_DIR}/telemetry" "$(dirname "${DEEP_TELEMETRY_ARCHIVE_INDEX}")"

  local -a run_artifacts=(
    "${METRICS_OUT}"
    "${CAPTURE_CONTEXT_OUT}"
    "${REFERENCE_CAPTURE}"
    "${CANDIDATE_CAPTURE}"
    "${REFERENCE_PNG}"
    "${CANDIDATE_PNG}"
    "${DIFF_OUT}"
    "${COMPARE_SIDE_BY_SIDE_OUT}"
  )
  for src in "${run_artifacts[@]}"; do
    copy_artifact_if_file "${src}" "${DEEP_ARCHIVE_DIR}/$(basename "${src}")"
  done

  local -a telemetry_artifacts=(
    "${CANDIDATE_TRACE_OUT}"
    "${CANDIDATE_PACKET_TRACE_OUT}"
    "${CANDIDATE_PACKET_REPLAY_OUT}"
    "${CANDIDATE_FRAME_FORENSICS_OUT}"
    "${CANDIDATE_FRAME_FORENSICS_SUMMARY_OUT}"
    "${CANDIDATE_FRAME_FORENSICS_ACTIVE_SUMMARY_OUT}"
    "${CANDIDATE_LAUNCH_LOG_OUT}"
    "${CANDIDATE_DEPTH_SUMMARY_OUT}"
    "${CANDIDATE_MISSING_REGION_FOCUS_OUT}"
    "${COMMAND_CENSUS_OUT}"
    "${COMMAND_CENSUS_MD_OUT}"
    "${CANDIDATE_HISTORY_MERGE_LOG_OUT}"
    "${CANDIDATE_OVERWRITE_LOG_OUT}"
    "${CANDIDATE_TRIANGLE_PACKET_LOG_OUT}"
    "${CANDIDATE_EXECUTOR_PRESENT_DUMP_OUT}"
    "${TELEMETRY_BUNDLE_OUT}"
  )
  for src in "${telemetry_artifacts[@]}"; do
    if [[ -n "${src}" ]]; then
      copy_artifact_if_file "${src}" "${DEEP_ARCHIVE_DIR}/telemetry/$(basename "${src}")"
    fi
  done

  if [[ -n "${DEVIATION_OUT_DIR}" && -d "${DEVIATION_OUT_DIR}" ]]; then
    local deviation_archive_dir
    deviation_archive_dir="${DEEP_ARCHIVE_DIR}/telemetry/$(basename "${DEVIATION_OUT_DIR}")"
    mkdir -p "${deviation_archive_dir}"
    cp -a "${DEVIATION_OUT_DIR}/." "${deviation_archive_dir}/"
  fi

  python3 - \
    "${DEEP_ARCHIVE_DIR}/run_meta.json" \
    "${SCENARIO_ID}" \
    "${DEEP_ARCHIVE_RUN_ID}" \
    "${DEEP_ARCHIVE_RUN_STAMP}" \
    "${DEEP_ARCHIVE_GIT_SHA}" \
    "${ROOT_DIR}" \
    "${RUN_ROOT}" \
    "${TELEMETRY_ROOT}" \
    "${VISUAL_COMPARE_EXIT_CODE}" \
    "${DEEP_REPLAY_EXIT_CODE}" \
    "${DEEP_FORENSICS_SUMMARY_EXIT_CODE}" \
    "${DEEP_FORENSICS_ACTIVE_SUMMARY_EXIT_CODE}" \
    "${METRICS_OUT}" \
    "${TELEMETRY_BUNDLE_OUT}" \
    "${CANDIDATE_MISSING_REGION_FOCUS_OUT}" <<'PY'
import json
import sys
from pathlib import Path

out_path = Path(sys.argv[1])
payload = {
    "scenario_id": sys.argv[2],
    "run_id": sys.argv[3],
    "run_stamp_utc": sys.argv[4],
    "git_commit_short": sys.argv[5],
    "root_dir": sys.argv[6],
    "run_root": sys.argv[7],
    "telemetry_root": sys.argv[8],
    "status": {
        "visual_compare_exit": int(sys.argv[9]),
        "packet_replay_exit": int(sys.argv[10]),
        "forensics_summary_exit": int(sys.argv[11]),
        "forensics_active_summary_exit": int(sys.argv[12]),
    },
    "artifacts": {
        "metrics": sys.argv[13],
        "telemetry_bundle": sys.argv[14],
        "missing_region_focus": sys.argv[15],
    },
}
out_path.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
PY

  if [[ ! -f "${DEEP_TELEMETRY_ARCHIVE_INDEX}" ]]; then
    printf "run_id\trun_stamp_utc\tscenario\tgit_commit_short\trmse\tmae\tcandidate_non_black_ratio\tcandidate_mean_luma\tarchive_dir\tbundle\n" > "${DEEP_TELEMETRY_ARCHIVE_INDEX}"
  fi

  python3 - \
    "${DEEP_TELEMETRY_ARCHIVE_INDEX}" \
    "${METRICS_OUT}" \
    "${DEEP_ARCHIVE_RUN_ID}" \
    "${DEEP_ARCHIVE_RUN_STAMP}" \
    "${SCENARIO_ID}" \
    "${DEEP_ARCHIVE_GIT_SHA}" \
    "${DEEP_ARCHIVE_DIR}" \
    "${TELEMETRY_BUNDLE_OUT}" <<'PY'
import json
import sys
from pathlib import Path

index_path = Path(sys.argv[1])
metrics_path = Path(sys.argv[2])
run_id = sys.argv[3]
run_stamp = sys.argv[4]
scenario_id = sys.argv[5]
git_sha = sys.argv[6]
archive_dir = sys.argv[7]
bundle_path = sys.argv[8]

rmse = ""
mae = ""
candidate_non_black_ratio = ""
candidate_mean_luma = ""
if metrics_path.is_file():
    try:
        data = json.loads(metrics_path.read_text(encoding="utf-8"))
        rmse = str(data.get("rmse", ""))
        mae = str(data.get("mae", ""))
        candidate_non_black_ratio = str(data.get("candidate_non_black_ratio", ""))
        candidate_mean_luma = str(data.get("candidate_mean_luma", ""))
    except Exception:
        pass

row = "\t".join(
    [
        run_id,
        run_stamp,
        scenario_id,
        git_sha,
        rmse,
        mae,
        candidate_non_black_ratio,
        candidate_mean_luma,
        archive_dir,
        bundle_path,
    ]
)
with index_path.open("a", encoding="utf-8") as f:
    f.write(row + "\n")
PY

  ln -sfn "${DEEP_ARCHIVE_DIR}" "${DEEP_TELEMETRY_ARCHIVE_ROOT}/${SCENARIO_ID}.latest"
  echo "deep telemetry archive: ${DEEP_ARCHIVE_DIR}"
  echo "deep telemetry index: ${DEEP_TELEMETRY_ARCHIVE_INDEX}"
}

resolve_overwrite_packet_ids_from_latest_focus() {
  if [[ "${DEEP_TELEMETRY}" != "1" || "${DEEP_TELEMETRY_OVERWRITE_LOG}" != "1" ]]; then
    return 0
  fi
  if [[ "${DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_FROM_LAST_FOCUS}" != "1" ]]; then
    return 0
  fi
  if [[ -n "${DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_IDS_EFFECTIVE}" ]]; then
    return 0
  fi

  local latest_link="${DEEP_TELEMETRY_ARCHIVE_ROOT}/${SCENARIO_ID}.latest"
  local latest_dir=""
  if [[ -e "${latest_link}" ]]; then
    latest_dir="$(readlink -f "${latest_link}" 2>/dev/null || true)"
  fi
  if [[ -z "${latest_dir}" || ! -d "${latest_dir}" ]]; then
    return 0
  fi

  local focus_json="${latest_dir}/telemetry/${SCENARIO_ID}.candidate.missing-region-focus.json"
  if [[ ! -f "${focus_json}" ]]; then
    return 0
  fi

  local packet_ids=""
  packet_ids="$(python3 - "${focus_json}" "${DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_MAX}" <<'PY'
import json
import sys
from pathlib import Path

focus_path = Path(sys.argv[1])
limit = max(1, int(sys.argv[2]))

try:
    payload = json.loads(focus_path.read_text(encoding="utf-8"))
except Exception:
    print("")
    raise SystemExit(0)

missing = payload.get("missing_write_attribution", {})
if not isinstance(missing, dict):
    print("")
    raise SystemExit(0)

auto_ids_raw = missing.get("auto_overwrite_packet_ids_suggested", [])
if isinstance(auto_ids_raw, list):
    auto_ids = []
    seen_auto = set()
    for value in auto_ids_raw:
        try:
            packet_id = int(value or 0)
        except Exception:
            packet_id = 0
        if packet_id <= 0 or packet_id in seen_auto:
            continue
        seen_auto.add(packet_id)
        auto_ids.append(packet_id)
        if len(auto_ids) >= limit:
            break
    if auto_ids:
        print(",".join(str(pid) for pid in auto_ids))
        raise SystemExit(0)

rows = missing.get("missing_with_write_packet_hits_ranked", [])
if not isinstance(rows, list) or not rows:
    rows = missing.get("missing_with_write_packet_hits", [])
if not isinstance(rows, list):
    print("")
    raise SystemExit(0)

def _is_truthy(value):
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        return value.strip().lower() in {"1", "true", "yes", "on"}
    return False

def _row_is_deprioritized(row):
    if _is_truthy(row.get("analysis_deprioritized", False)):
        return True
    coverage = row.get("triangle_packet_log_coverage")
    if coverage != "matched":
        return False
    try:
        sample_candidates = int(row.get("triangle_sample_candidates", 0) or 0)
    except Exception:
        sample_candidates = 0
    try:
        writes = int(row.get("triangle_writes", 0) or 0)
    except Exception:
        writes = 0
    if sample_candidates > 0 or writes > 0:
        return False
    reason = row.get("triangle_dominant_zero_sample_reason")
    return isinstance(reason, str) and reason in {
        "y_range",
        "x_edge",
        "scissor_field",
        "bounds",
        "degenerate",
        "other",
    }

packet_ids = []
seen = set()
for row in rows:
    if not isinstance(row, dict):
        continue
    if _row_is_deprioritized(row):
        continue
    try:
        packet_id = int(row.get("source_packet_id", 0) or 0)
    except Exception:
        packet_id = 0
    if packet_id <= 0 or packet_id in seen:
        continue
    seen.add(packet_id)
    packet_ids.append(packet_id)
    if len(packet_ids) >= limit:
        break

if len(packet_ids) < min(8, limit):
    for row in rows:
        if not isinstance(row, dict):
            continue
        try:
            packet_id = int(row.get("source_packet_id", 0) or 0)
        except Exception:
            packet_id = 0
        if packet_id <= 0 or packet_id in seen:
            continue
        seen.add(packet_id)
        packet_ids.append(packet_id)
        if len(packet_ids) >= limit:
            break

print(",".join(str(pid) for pid in packet_ids))
PY
)"

  if [[ -z "${packet_ids}" ]]; then
    return 0
  fi

  DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_IDS_EFFECTIVE="${packet_ids}"
  echo "==> [telemetry] auto-selected overwrite packet ids from latest focus: ${DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_IDS_EFFECTIVE}"
  if [[ "${DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_ALL_WRITES}" == "0" ]]; then
    DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_ALL_WRITES="1"
    echo "==> [telemetry] enabling include-all-writes for hotspot packet stage coverage."
  fi
}

track_knob_fingerprint() {
  if [[ "${KNOB_TRACK_ENABLE}" != "1" ]]; then
    return 0
  fi

  mkdir -p "$(dirname "${KNOB_TRACK_FILE}")"
  local -a knob_kv=(
    "profile=${PROFILE}"
    "capture_scale_div=${CAPTURE_SCALE_DIV}"
    "scenario_frames=${SCENARIO_FRAMES}"
    "effective_frames=${FRAMES}"
    "dumpfb_flip_y=${DUMPFB_FLIP_Y}"
    "rvk2_present_flip_y=${RVK2_PRESENT_FLIP_Y}"
    "deep_telemetry=${DEEP_TELEMETRY}"
    "deep_replay_stateful=${DEEP_TELEMETRY_REPLAY_STATEFUL}"
    "deep_replay_jobs=${DEEP_TELEMETRY_REPLAY_JOBS}"
    "deep_diff_mode=${DEEP_TELEMETRY_DIFF_MODE}"
    "deep_diff_threshold=${DEEP_TELEMETRY_DIFF_THRESHOLD}"
    "deep_diff_min_area=${DEEP_TELEMETRY_DIFF_MIN_AREA}"
    "deep_diff_max_boxes=${DEEP_TELEMETRY_DIFF_MAX_BOXES}"
    "deep_history_merge_log=${DEEP_TELEMETRY_HISTORY_MERGE_LOG}"
    "deep_overwrite_log=${DEEP_TELEMETRY_OVERWRITE_LOG}"
    "deep_overwrite_include_all=${DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_ALL_WRITES}"
    "deep_overwrite_include_texel_detail=${DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_TEXEL_DETAIL}"
    "deep_overwrite_packet_ids=${DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_IDS_EFFECTIVE}"
    "deep_triangle_packet_log=${DEEP_TELEMETRY_TRIANGLE_PACKET_LOG}"
    "deep_triangle_packet_log_limit=${DEEP_TELEMETRY_TRIANGLE_PACKET_LOG_LIMIT}"
    "deep_command_census=${DEEP_TELEMETRY_COMMAND_CENSUS}"
    "deep_missing_region_history_window=${DEEP_TELEMETRY_MISSING_REGION_HISTORY_WINDOW}"
    "deep_missing_region_max_overlap=${DEEP_TELEMETRY_MISSING_REGION_MAX_ADDRESS_OVERLAP}"
    "rvk2_enable_surface_history_bootstrap=${RVK2_ENABLE_SURFACE_HISTORY_BOOTSTRAP}"
    "rvk2_enable_cross_surface_bootstrap=${RVK2_ENABLE_CROSS_SURFACE_BOOTSTRAP}"
    "rvk2_disable_vi_history_present=${RVK2_DISABLE_VI_HISTORY_PRESENT}"
    "rvk2_prefer_live_surface=${RVK2_PREFER_LIVE_SURFACE_OVER_HISTORY}"
  )
  local -a knob_cmd=(
    python3 "${ROOT_DIR}/scripts/rvk2_knob_history.py"
    fingerprint
    --output-json "${KNOB_SNAPSHOT_OUT}"
    --scenario-id "${SCENARIO_ID}"
    --profile "${PROFILE}"
  )
  local kv_entry
  for kv_entry in "${knob_kv[@]}"; do
    knob_cmd+=(--kv="${kv_entry}")
  done

  KNOB_FINGERPRINT="$("${knob_cmd[@]}")"
  cp -f "${KNOB_SNAPSHOT_OUT}" "${KNOB_SNAPSHOT_LATEST_OUT}"
  echo "knob fingerprint: ${KNOB_FINGERPRINT}"
  echo "knob snapshot: ${KNOB_SNAPSHOT_OUT}"

  local recent_json
  recent_json="$(
    python3 "${ROOT_DIR}/scripts/rvk2_knob_history.py" \
      recent \
      --history "${KNOB_TRACK_FILE}" \
      --scenario-id "${SCENARIO_ID}" \
      --fingerprint "${KNOB_FINGERPRINT}" \
      --window "${KNOB_TRACK_WINDOW}"
  )"
  local recent_same_count
  recent_same_count="$(python3 - "${recent_json}" <<'PY'
import json
import sys

try:
    payload = json.loads(sys.argv[1])
except Exception:
    print("0")
    raise SystemExit(0)
print(int(payload.get("same_fingerprint_count", 0) or 0))
PY
)"
  if [[ "${recent_same_count}" != "0" ]]; then
    echo "WARN: knob fingerprint already appears ${recent_same_count} time(s) in last ${KNOB_TRACK_WINDOW} runs for ${SCENARIO_ID}." >&2
  fi
}

record_knob_history() {
  if [[ "${KNOB_TRACK_ENABLE}" != "1" || -z "${KNOB_FINGERPRINT}" ]]; then
    return 0
  fi
  python3 "${ROOT_DIR}/scripts/rvk2_knob_history.py" \
    record \
    --history "${KNOB_TRACK_FILE}" \
    --run-stamp "${DEEP_ARCHIVE_RUN_STAMP}" \
    --scenario-id "${SCENARIO_ID}" \
    --profile "${PROFILE}" \
    --fingerprint "${KNOB_FINGERPRINT}" \
    --deep-telemetry "${DEEP_TELEMETRY}" \
    --visual-exit "${VISUAL_COMPARE_EXIT_CODE}" \
    --git-sha "${DEEP_ARCHIVE_GIT_SHA}" \
    --snapshot "${KNOB_SNAPSHOT_OUT}" \
    --metrics "${METRICS_OUT}"
  echo "knob history: ${KNOB_TRACK_FILE}"
  python3 "${ROOT_DIR}/scripts/rvk2_knob_history.py" \
    summary \
    --history "${KNOB_TRACK_FILE}" \
    --scenario-id "${SCENARIO_ID}" \
    --limit "${KNOB_TRACK_SUMMARY_LIMIT}" > "${RUN_ROOT}/${SCENARIO_ID}.knob-history-recent.tsv"
  echo "knob recent summary: ${RUN_ROOT}/${SCENARIO_ID}.knob-history-recent.tsv"
}

resolve_overwrite_packet_ids_from_latest_focus
track_knob_fingerprint
echo "profile: ${PROFILE}"
if [[ "${DRY_RUN}" == "1" ]]; then
  rm -f "${DRY_RUN_CAPTURE_RECORDS_FILE}"
fi

if [[ "${REFRESH_REFERENCE}" == "1" || ! -s "${REFERENCE_CAPTURE}" ]]; then
  warn_reference_dumpfb_incompatibility
  if [[ ! -f "${REFERENCE_PLUGIN}" ]]; then
    echo "ERROR: reference plugin not found: ${REFERENCE_PLUGIN}" >&2
    exit 2
  fi
  echo "==> [compare] capturing reference (${SCENARIO_ID})"
  echo "    plugin: ${REFERENCE_PLUGIN}"
  echo "    core:   ${REFERENCE_CORELIB}"
  echo "    capture:${CAPTURE_METHOD_TAG} (dumpfb-preset)"
  capture_plugin "reference" "${REFERENCE_PLUGIN}" "${REFERENCE_CAPTURE}"
else
  echo "==> [compare] using cached reference capture: ${REFERENCE_CAPTURE}"
  if [[ "${REQUIRE_NON_BLACK_CAPTURE}" == "1" && "${VALIDATE_CACHED_REFERENCE_CAPTURE}" == "1" ]]; then
    if [[ "${REFERENCE_DUMPFB_INCOMPATIBLE}" == "1" ]]; then
      warn_reference_dumpfb_incompatibility
      echo "WARN: skipping cached reference non-black validation for GLideN64 dumpfb path." >&2
    else
      if ! capture_has_content "${REFERENCE_CAPTURE}"; then
        if [[ ! -f "${REFERENCE_PLUGIN}" ]]; then
          echo "ERROR: cached reference capture is mostly black and reference plugin is unavailable for recapture: ${REFERENCE_PLUGIN}" >&2
          exit 2
        fi
        echo "WARN: cached reference capture appears mostly black; recapturing reference." >&2
        capture_plugin "reference" "${REFERENCE_PLUGIN}" "${REFERENCE_CAPTURE}"
      fi
    fi
  fi
fi

echo "==> [compare] capturing candidate (${SCENARIO_ID})"
echo "    plugin: ${CANDIDATE_PLUGIN}"
echo "    core:   ${CANDIDATE_CORELIB}"
echo "    capture:${CAPTURE_METHOD_TAG} (dumpfb-preset)"
capture_plugin "candidate" "${CANDIDATE_PLUGIN}" "${CANDIDATE_CAPTURE}"
if [[ "${DRY_RUN}" == "1" ]]; then
  emit_dry_run_plan_and_exit
fi

python3 - "${CAPTURE_CONTEXT_OUT}" \
  "${SCENARIO_ID}" \
  "${REFERENCE_PLUGIN}" \
  "${REFERENCE_CORELIB}" \
  "${CAPTURE_METHOD_TAG}" \
  "${REFERENCE_CAPTURE}" \
  "${CANDIDATE_PLUGIN}" \
  "${CANDIDATE_CORELIB}" \
  "${CAPTURE_METHOD_TAG}" \
  "${CANDIDATE_CAPTURE}" <<'PY'
import json
import sys
from pathlib import Path

out_path = Path(sys.argv[1])
payload = {
    "scenario_id": sys.argv[2],
    "reference": {
        "plugin": sys.argv[3],
        "corelib": sys.argv[4],
        "capture_method": sys.argv[5],
        "capture_path": sys.argv[6],
    },
    "candidate": {
        "plugin": sys.argv[7],
        "corelib": sys.argv[8],
        "capture_method": sys.argv[9],
        "capture_path": sys.argv[10],
    },
}
out_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
print(f"capture context: {out_path}")
PY

if python3 - "${REFERENCE_CAPTURE}" "${CANDIDATE_CAPTURE}" "${DIFF_OUT}" "${METRICS_OUT}" "${RMSE_MAX}" "${MAE_MAX}" "${VISUAL_GATE}" <<'PY'
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ref_path = Path(sys.argv[1])
test_path = Path(sys.argv[2])
diff_path = Path(sys.argv[3])
metrics_path = Path(sys.argv[4])
rmse_limit = float(sys.argv[5]) if sys.argv[5] else None
mae_limit = float(sys.argv[6]) if sys.argv[6] else None
gate_enabled = sys.argv[7] == "1"

ref = np.asarray(Image.open(ref_path).convert("RGB"), dtype=np.float32) / 255.0
test = np.asarray(Image.open(test_path).convert("RGB"), dtype=np.float32) / 255.0

if ref.shape != test.shape:
    raise SystemExit(
        f"ERROR: capture shape mismatch: reference={ref.shape}, candidate={test.shape}"
    )

abs_diff = np.abs(ref - test)
rmse = float(np.sqrt(np.mean((ref - test) ** 2)))
mae = float(np.mean(abs_diff))
max_diff = float(np.max(abs_diff))

diff_u8 = np.clip(abs_diff * 255.0, 0.0, 255.0).astype(np.uint8)
Image.fromarray(diff_u8, mode="RGB").save(diff_path)

reference_non_black_ratio = float(np.any(ref > 0.0, axis=2).mean())
candidate_non_black_ratio = float(np.any(test > 0.0, axis=2).mean())
ref_luma = 0.2126 * ref[..., 0] + 0.7152 * ref[..., 1] + 0.0722 * ref[..., 2]
candidate_luma = 0.2126 * test[..., 0] + 0.7152 * test[..., 1] + 0.0722 * test[..., 2]
reference_mean_luma = float(ref_luma.mean())
candidate_mean_luma = float(candidate_luma.mean())

metrics = {
    "reference_capture": str(ref_path),
    "candidate_capture": str(test_path),
    "diff_image": str(diff_path),
    "rmse": rmse,
    "mae": mae,
    "max_abs_diff": max_diff,
    "reference_non_black_ratio": reference_non_black_ratio,
    "candidate_non_black_ratio": candidate_non_black_ratio,
    "reference_mean_luma": reference_mean_luma,
    "candidate_mean_luma": candidate_mean_luma,
    "rmse_limit": rmse_limit,
    "mae_limit": mae_limit,
    "gate_enabled": gate_enabled,
}

violations = []
if gate_enabled:
    if rmse_limit is not None and rmse > rmse_limit:
        violations.append(f"rmse {rmse:.6f} > limit {rmse_limit:.6f}")
    if mae_limit is not None and mae > mae_limit:
        violations.append(f"mae {mae:.6f} > limit {mae_limit:.6f}")

metrics["pass"] = len(violations) == 0
metrics["violations"] = violations
metrics_path.write_text(json.dumps(metrics, indent=2), encoding="utf-8")

print(f"visual compare metrics: rmse={rmse:.6f} mae={mae:.6f} max_abs_diff={max_diff:.6f}")
print(
    "capture content metrics: "
    f"reference_non_black_ratio={reference_non_black_ratio:.6f} "
    f"candidate_non_black_ratio={candidate_non_black_ratio:.6f} "
    f"reference_mean_luma={reference_mean_luma:.6f} "
    f"candidate_mean_luma={candidate_mean_luma:.6f}"
)
print(f"metrics json: {metrics_path}")
print(f"diff image: {diff_path}")

if violations:
    for item in violations:
        print(f"ERROR: {item}", file=sys.stderr)
    raise SystemExit(1)
PY
then
  :
else
  VISUAL_COMPARE_EXIT_CODE="$?"
  if [[ "${DEEP_TELEMETRY}" == "1" ]]; then
    echo "WARN: visual parity gate failed; continuing to emit deep telemetry bundle." >&2
  fi
fi

python3 - "${REFERENCE_CAPTURE}" "${CANDIDATE_CAPTURE}" "${REFERENCE_PNG}" "${CANDIDATE_PNG}" <<'PY'
from pathlib import Path
from PIL import Image
import sys

ref_ppm = Path(sys.argv[1])
cand_ppm = Path(sys.argv[2])
ref_png = Path(sys.argv[3])
cand_png = Path(sys.argv[4])

ref_png.parent.mkdir(parents=True, exist_ok=True)
Image.open(ref_ppm).convert("RGB").save(ref_png)
Image.open(cand_ppm).convert("RGB").save(cand_png)
print(f"reference png: {ref_png}")
print(f"candidate png: {cand_png}")
PY

if [[ "${AUTO_COMPARE_VIEW}" == "1" ]]; then
  if rvk2_build_side_by_side_image "${REFERENCE_PNG}" "${CANDIDATE_PNG}" "${COMPARE_SIDE_BY_SIDE_OUT}"; then
    echo "auto compare image: ${COMPARE_SIDE_BY_SIDE_OUT}"
    rvk2_close_eog_view "${COMPARE_VIEWER_PID_FILE}" "${AUTO_COMPARE_CLOSE_ALL_EOG}"
    rvk2_open_eog_view "${COMPARE_SIDE_BY_SIDE_OUT}" "${COMPARE_VIEWER_PID_FILE}"
  fi
fi

if [[ "${CAPTURE_DEPTH_SUMMARY}" == "1" ]]; then
  ref_depth_summary="${RUN_ROOT}/${SCENARIO_ID}.reference.depth-blit-summary.json"
  if [[ "${DEEP_TELEMETRY}" == "1" ]]; then
    cand_depth_summary="${CANDIDATE_DEPTH_SUMMARY_OUT}"
  else
    cand_depth_summary="${RUN_ROOT}/${SCENARIO_ID}.candidate.depth-blit-summary.json"
  fi
  if [[ -f "${ref_depth_summary}" ]]; then
    echo "depth blit summary (reference): ${ref_depth_summary}"
  fi
  if [[ -f "${cand_depth_summary}" ]]; then
    echo "depth blit summary (candidate): ${cand_depth_summary}"
  fi
fi

if [[ "${DEEP_TELEMETRY}" == "1" ]]; then
  echo "==> [telemetry] replay packet trace"
  if [[ -s "${CANDIDATE_PACKET_TRACE_OUT}" ]]; then
    replay_args=(
      "${ROOT_DIR}/scripts/rvk2_packet_trace_replay.py"
      --input "${CANDIDATE_PACKET_TRACE_OUT}"
      --json-out "${CANDIDATE_PACKET_REPLAY_OUT}"
      --jobs "${DEEP_TELEMETRY_REPLAY_JOBS}"
    )
    if [[ -s "${CANDIDATE_FRAME_FORENSICS_OUT}" ]]; then
      replay_args+=(--forensics-file "${CANDIDATE_FRAME_FORENSICS_OUT}")
    fi
    if [[ "${DEEP_TELEMETRY_REPLAY_STATEFUL}" == "1" ]]; then
      replay_args+=(--stateful-frames)
    fi
    if [[ "${DEEP_TELEMETRY_REPLAY_STRICT}" == "1" ]]; then
      replay_args+=(--strict)
    fi
    if python3 "${replay_args[@]}"; then
      DEEP_REPLAY_EXIT_CODE="0"
    else
      DEEP_REPLAY_EXIT_CODE="$?"
      echo "WARN: packet trace replay reported failures (exit=${DEEP_REPLAY_EXIT_CODE})." >&2
    fi
  else
    DEEP_REPLAY_EXIT_CODE="2"
    echo "WARN: packet trace missing for replay: ${CANDIDATE_PACKET_TRACE_OUT}" >&2
  fi

  echo "==> [telemetry] summarize frame forensics"
  if [[ -s "${CANDIDATE_FRAME_FORENSICS_OUT}" ]]; then
    if python3 "${ROOT_DIR}/scripts/rvk2_forensics_summary.py" --input "${CANDIDATE_FRAME_FORENSICS_OUT}" > "${CANDIDATE_FRAME_FORENSICS_SUMMARY_OUT}"; then
      DEEP_FORENSICS_SUMMARY_EXIT_CODE="0"
    else
      DEEP_FORENSICS_SUMMARY_EXIT_CODE="$?"
      echo "WARN: full forensics summary failed (exit=${DEEP_FORENSICS_SUMMARY_EXIT_CODE})." >&2
    fi
    if python3 "${ROOT_DIR}/scripts/rvk2_forensics_summary.py" --input "${CANDIDATE_FRAME_FORENSICS_OUT}" --active-only > "${CANDIDATE_FRAME_FORENSICS_ACTIVE_SUMMARY_OUT}"; then
      DEEP_FORENSICS_ACTIVE_SUMMARY_EXIT_CODE="0"
    else
      DEEP_FORENSICS_ACTIVE_SUMMARY_EXIT_CODE="$?"
      echo "WARN: active-only forensics summary failed (exit=${DEEP_FORENSICS_ACTIVE_SUMMARY_EXIT_CODE})." >&2
    fi
  else
    DEEP_FORENSICS_SUMMARY_EXIT_CODE="2"
    DEEP_FORENSICS_ACTIVE_SUMMARY_EXIT_CODE="2"
    echo "WARN: frame forensics file missing: ${CANDIDATE_FRAME_FORENSICS_OUT}" >&2
  fi

  if [[ "${DEEP_TELEMETRY_DIFF_PLAYBOOK}" == "1" ]]; then
    echo "==> [telemetry] build deviation playbook artifacts"
    diff_ignore_args=()
    if [[ -n "${DEEP_TELEMETRY_DIFF_IGNORE_BOXES}" ]]; then
      IFS=';' read -r -a diff_ignore_boxes <<< "${DEEP_TELEMETRY_DIFF_IGNORE_BOXES}"
      for raw_box in "${diff_ignore_boxes[@]}"; do
        box="${raw_box//[[:space:]]/}"
        if [[ -n "${box}" ]]; then
          diff_ignore_args+=(--ignore-box "${box}")
        fi
      done
    fi
    if python3 "${ROOT_DIR}/scripts/rvk2_image_diff_playbook.py" \
      --ref "${REFERENCE_PNG}" \
      --test "${CANDIDATE_PNG}" \
      --outdir "${DEVIATION_OUT_DIR}" \
      --mode "${DEEP_TELEMETRY_DIFF_MODE}" \
      --threshold "${DEEP_TELEMETRY_DIFF_THRESHOLD}" \
      --min-area "${DEEP_TELEMETRY_DIFF_MIN_AREA}" \
      --max-boxes "${DEEP_TELEMETRY_DIFF_MAX_BOXES}" \
      --dilate "${DEEP_TELEMETRY_DIFF_DILATE}" \
      --ref-non-black-threshold "${DEEP_TELEMETRY_DIFF_REF_NONBLACK_THRESHOLD}" \
      --test-non-black-threshold "${DEEP_TELEMETRY_DIFF_TEST_NONBLACK_THRESHOLD}" \
      "${diff_ignore_args[@]}"; then
      :
    else
      echo "WARN: deviation playbook artifact generation failed." >&2
    fi
  fi

  if [[ "${DEEP_TELEMETRY_COMMAND_CENSUS}" == "1" ]]; then
    echo "==> [telemetry] command census"
    if [[ -s "${CANDIDATE_PACKET_TRACE_OUT}" ]]; then
      census_args=(
        "${ROOT_DIR}/scripts/rvk2_packet_command_census.py"
        --input "${CANDIDATE_PACKET_TRACE_OUT}"
        --json-out "${COMMAND_CENSUS_OUT}"
        --md-out "${COMMAND_CENSUS_MD_OUT}"
        --focus-window "${DEEP_TELEMETRY_COMMAND_FOCUS_WINDOW}"
      )
      if [[ -s "${CANDIDATE_PACKET_REPLAY_OUT}" ]]; then
        census_args+=(--replay "${CANDIDATE_PACKET_REPLAY_OUT}")
      fi
      if python3 "${census_args[@]}"; then
        :
      else
        echo "WARN: command census generation failed." >&2
      fi
    else
      echo "WARN: packet trace missing for command census: ${CANDIDATE_PACKET_TRACE_OUT}" >&2
    fi
  fi

  if [[ -s "${CANDIDATE_PACKET_TRACE_OUT}" && -s "${DIFF_PLAYBOOK_SUMMARY_OUT}" ]]; then
    echo "==> [telemetry] missing-region focus census"
    if python3 "${ROOT_DIR}/scripts/rvk2_missing_region_focus.py" \
      --packet-trace "${CANDIDATE_PACKET_TRACE_OUT}" \
      --diff-summary "${DIFF_PLAYBOOK_SUMMARY_OUT}" \
      --forensics "${CANDIDATE_FRAME_FORENSICS_OUT}" \
      --triangle-packet-log "${CANDIDATE_TRIANGLE_PACKET_LOG_OUT}" \
      --max-hit-samples "${DEEP_TELEMETRY_MISSING_REGION_MAX_HIT_SAMPLES}" \
      --history-frame-window "${DEEP_TELEMETRY_MISSING_REGION_HISTORY_WINDOW}" \
      --max-address-overlap "${DEEP_TELEMETRY_MISSING_REGION_MAX_ADDRESS_OVERLAP}" \
      --output "${CANDIDATE_MISSING_REGION_FOCUS_OUT}"; then
      :
    else
      echo "WARN: missing-region focus census generation failed." >&2
    fi
  fi

  python3 "${ROOT_DIR}/scripts/rvk2_telemetry_bundle.py" \
    --scenario-id "${SCENARIO_ID}" \
    --output "${TELEMETRY_BUNDLE_OUT}" \
    --metrics "${METRICS_OUT}" \
    --capture-context "${CAPTURE_CONTEXT_OUT}" \
    --reference-capture "${REFERENCE_CAPTURE}" \
    --candidate-capture "${CANDIDATE_CAPTURE}" \
    --reference-png "${REFERENCE_PNG}" \
    --candidate-png "${CANDIDATE_PNG}" \
    --diff-image "${DIFF_OUT}" \
    --trace-file "${CANDIDATE_TRACE_OUT}" \
    --packet-trace "${CANDIDATE_PACKET_TRACE_OUT}" \
    --packet-replay "${CANDIDATE_PACKET_REPLAY_OUT}" \
    --forensics "${CANDIDATE_FRAME_FORENSICS_OUT}" \
    --forensics-summary "${CANDIDATE_FRAME_FORENSICS_SUMMARY_OUT}" \
    --forensics-summary-active "${CANDIDATE_FRAME_FORENSICS_ACTIVE_SUMMARY_OUT}" \
    --depth-summary "${CANDIDATE_DEPTH_SUMMARY_OUT}" \
    --launch-log "${CANDIDATE_LAUNCH_LOG_OUT}" \
    --diff-playbook-summary "${DIFF_PLAYBOOK_SUMMARY_OUT}" \
    --diff-playbook-boxes "${DIFF_PLAYBOOK_BOXES_OUT}" \
    --diff-playbook-snippet "${DIFF_PLAYBOOK_SNIPPET_OUT}" \
    --missing-region-focus "${CANDIDATE_MISSING_REGION_FOCUS_OUT}" \
    --history-merge-log "${CANDIDATE_HISTORY_MERGE_LOG_OUT}" \
    --overwrite-log "${CANDIDATE_OVERWRITE_LOG_OUT}" \
    --triangle-packet-log "${CANDIDATE_TRIANGLE_PACKET_LOG_OUT}" \
    --executor-present-dump "${CANDIDATE_EXECUTOR_PRESENT_DUMP_OUT}" \
    --command-census "${COMMAND_CENSUS_OUT}" \
    --packet-replay-exit "${DEEP_REPLAY_EXIT_CODE}" \
    --forensics-summary-exit "${DEEP_FORENSICS_SUMMARY_EXIT_CODE}" \
    --forensics-summary-active-exit "${DEEP_FORENSICS_ACTIVE_SUMMARY_EXIT_CODE}"
  if [[ -f "${DIFF_PLAYBOOK_SNIPPET_OUT}" ]]; then
    echo "deviation playbook snippet: ${DIFF_PLAYBOOK_SNIPPET_OUT}"
  fi
  if [[ -f "${COMMAND_CENSUS_MD_OUT}" ]]; then
    echo "command census: ${COMMAND_CENSUS_MD_OUT}"
  fi
  echo "telemetry bundle: ${TELEMETRY_BUNDLE_OUT}"
  if [[ "${DEEP_TELEMETRY_REQUIRE_LIVE_PRESENT_SURFACE}" == "1" ]]; then
    if ! python3 - "${TELEMETRY_BUNDLE_OUT}" <<'PY'
import json
import sys
from pathlib import Path

bundle_path = Path(sys.argv[1])
if not bundle_path.is_file():
    print(f"ERROR: telemetry bundle not found for handoff guard: {bundle_path}", file=sys.stderr)
    raise SystemExit(2)

try:
    payload = json.loads(bundle_path.read_text(encoding="utf-8"))
except Exception as exc:
    print(f"ERROR: failed to parse telemetry bundle for handoff guard: {exc}", file=sys.stderr)
    raise SystemExit(2)

hard_faults = payload.get("hard_faults", [])
if isinstance(hard_faults, list):
    for fault in hard_faults:
        if isinstance(fault, str) and "present-surface handoff fault" in fault:
            print(f"ERROR: {fault}", file=sys.stderr)
            raise SystemExit(1)

print("telemetry hard-fault guard: no present-surface handoff fault detected")
PY
    then
      if [[ "${VISUAL_COMPARE_EXIT_CODE}" == "0" ]]; then
        VISUAL_COMPARE_EXIT_CODE=1
      fi
    fi
  fi
fi

archive_deep_telemetry_run
record_knob_history

if [[ "${VISUAL_COMPARE_EXIT_CODE}" != "0" ]]; then
  exit "${VISUAL_COMPARE_EXIT_CODE}"
fi
