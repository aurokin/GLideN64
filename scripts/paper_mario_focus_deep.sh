#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT_DIR}/scripts/lib/validation.sh"

SCENARIO_ID="${REALITYVK_PM_SCENARIO_ID:-paper_mario_intro}"
FRAMES_OVERRIDE="${REALITYVK_PM_FRAMES_OVERRIDE:-120}"
AUTO_COMPARE_VIEW="${REALITYVK_PM_AUTO_COMPARE_VIEW:-1}"
OVERWRITE_LOG_LIMIT="${REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_LIMIT:-250000}"
AUTO_PACKET_IDS_MAX="${REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_MAX:-64}"

usage() {
  cat <<'EOF_USAGE'
Usage:
  paper_mario_focus_deep.sh [options]

Options:
  --scenario-id <id>     Scenario id (default: paper_mario_intro)
  --frames <n>           Frame override for deep run (default: 120)
  --auto-view <0|1>      Open compare viewer (default: 1)
  --log-limit <n>        Overwrite log row limit (default: 250000)
  --packet-max <n>       Max packet ids auto-selected from latest focus (default: 64)
  -h, --help             Show this help
EOF_USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --scenario-id)
      SCENARIO_ID="$2"
      shift 2
      ;;
    --frames)
      FRAMES_OVERRIDE="$2"
      shift 2
      ;;
    --auto-view)
      AUTO_COMPARE_VIEW="$2"
      shift 2
      ;;
    --log-limit)
      OVERWRITE_LOG_LIMIT="$2"
      shift 2
      ;;
    --packet-max)
      AUTO_PACKET_IDS_MAX="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "ERROR: unknown option '$1'" >&2
      usage
      exit 2
      ;;
  esac
done

rvk2_require_uint_ge "--frames" "${FRAMES_OVERRIDE}" 0
rvk2_require_bool "--auto-view" "${AUTO_COMPARE_VIEW}"
rvk2_require_uint_ge "--log-limit" "${OVERWRITE_LOG_LIMIT}" 0
rvk2_require_uint_ge "--packet-max" "${AUTO_PACKET_IDS_MAX}" 1

echo "==> [focus-deep] scenario=${SCENARIO_ID} frames=${FRAMES_OVERRIDE}"
echo "==> [focus-deep] auto packet IDs from latest missing-region focus enabled"

env \
  REALITYVK_PM_SCENARIO_ID="${SCENARIO_ID}" \
  REALITYVK_PM_PROFILE=deep \
  REALITYVK_PM_DEEP_TELEMETRY=1 \
  REALITYVK_PM_DEEP_TELEMETRY_ARCHIVE=1 \
  REALITYVK_PM_DEEP_TELEMETRY_REPLAY_STATEFUL=1 \
  REALITYVK_PM_DEEP_TELEMETRY_REPLAY_JOBS=0 \
  REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG=1 \
  REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_FROM_LAST_FOCUS=1 \
  REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_PACKET_IDS= \
  REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_INCLUDE_ALL_WRITES=1 \
  REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_LIMIT="${OVERWRITE_LOG_LIMIT}" \
  REALITYVK_PM_DEEP_TELEMETRY_OVERWRITE_LOG_AUTO_PACKET_IDS_MAX="${AUTO_PACKET_IDS_MAX}" \
  REALITYVK_PM_VISUAL_GATE=0 \
  REALITYVK_PM_AUTO_COMPARE_VIEW="${AUTO_COMPARE_VIEW}" \
  REALITYVK_PM_FRAMES_OVERRIDE="${FRAMES_OVERRIDE}" \
  "${ROOT_DIR}/scripts/paper_mario_parity.sh"
